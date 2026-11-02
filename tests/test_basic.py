# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

import pytest

from simlib import CliError


def test_tool_cli(tool):
    assert tool.cmd("version")["app"] == "ha_tool"
    st = tool.cmd("status")
    assert st["mode"] == "none" and st["identity"] == "none" and st["source"] == "idle"
    assert st["broadcasts_active"] == "0" and st["broadcasts_configured"] == "0"
    assert "identities" not in st.values
    assert tool.cmd("status --verbose")["identities"] == "0"

    rep = tool.cmd("bogus", check=False)
    assert not rep.ok


def test_tool_virtual_devices(tool):
    a = tool.cmd("bonding create PhoneA")
    b = tool.cmd("bonding create")
    assert a["addr"] != b["addr"]
    assert a["name"] == "PhoneA"

    listing = tool.cmd("bonding list")
    assert listing["count"] == "2"
    assert {d["id"] for d in listing.data} == {a["id"], b["id"]}

    tool.cmd(f"bonding delete {a['id']}")
    assert tool.cmd("bonding list")["count"] == "1"

    # The deleted identity slot is reused
    c = tool.cmd("bonding create PhoneC")
    assert c["id"] == a["id"]
    assert c["addr"] != a["addr"]

    with pytest.raises(CliError):
        tool.cmd("bonding pair 7")


def test_tool_broadcast_lifecycle(tool):
    rep = tool.cmd(
        "broadcast 1 create name=Hall preset=24_2 bis=2 code=secret pbp=hq lang=pol id=0x123456"
    )
    assert rep["broadcast_id"] == "0x123456" and rep["encrypted"] == "1" and rep["pbp"] == "hq"
    tool.cmd("broadcast 1 bis tone 2 freq=700")
    tool.cmd("broadcast 1 start")
    tool.wait_evt("broadcast_started", bis=2, timeout=15)
    tool.cmd("broadcast 1 subgroup set 1 program=News")
    st = tool.cmd("broadcast 1 show")
    assert st["state"] == "active" and st["bis"] == "2"
    assert [d["tone"] for d in st.data if d["_tag"] == "BIS"] == ["500", "700"]
    assert st.data[0]["_tag"] == "SUBGROUP" and st.data[0]["lang"] == "pol"
    tool.cmd("broadcast 1 stop")
    tool.wait_evt("broadcast_stopped", timeout=15)
    tool.cmd("broadcast 1 delete")
    rep = tool.cmd("broadcast 1 show", check=False)
    assert rep.errno == -2 and rep.values["hint"] == "broadcast 1 create"


def test_modes(tool):
    """unicast (bonding + le-source) or broadcast: the first command picks the mode, the other
    area is refused until 'mode' switches, and switching clears the whole previous context."""
    assert tool.cmd("mode")["mode"] == "none"

    a = tool.cmd("bonding create Phone")["id"]
    assert tool.cmd("mode")["mode"] == "unicast"
    rep = tool.cmd("broadcast 1 create", check=False)
    assert not rep.ok and rep.errno == -16 and rep.values["hint"] == "mode broadcast"
    rep = tool.cmd("le-source music start", check=False)
    assert rep.errno == -107 and "le-source attach" in rep.values["hint"]  # nothing attached

    # Read-only commands work in any mode
    assert tool.cmd("broadcast list")["count"] == "0"
    assert tool.cmd("le-source status")["state"] == "idle"

    # bonding and le-source share the unicast session
    assert tool.cmd("le-source attach Phone")["id"] == a
    assert tool.cmd("status")["identity"] == "Phone"

    # -> broadcast: identities, bonds and the source are gone
    tool.cmd("mode broadcast")
    assert tool.cmd("status")["mode"] == "broadcast"
    assert tool.cmd("bonding list")["count"] == "0"
    assert tool.cmd("le-source attach Phone", check=False).errno == -2
    tool.cmd("broadcast 1 create bis=1")

    # -> unicast: the broadcast is gone
    tool.cmd("mode unicast")
    assert tool.cmd("broadcast list")["count"] == "0"
    assert tool.cmd("bonding list")["count"] == "0"

    rep = tool.cmd("mode none", check=False)
    assert rep.errno == -22 and "reset" in rep.values["hint"]
    tool.cmd("reset")
    assert tool.cmd("mode")["mode"] == "none"


def test_failed_command_keeps_mode(tool):
    """A command entering its mode from none and then failing leaves the board in none;
    a successful one keeps the mode."""
    m = tool.mark()
    for line in (
        "bonding create io=bogus",
        "broadcast budget preset=bogus",
        "le-source volume set 999",
    ):
        assert not tool.cmd(line, check=False).ok, line
        assert tool.cmd("mode")["mode"] == "none", line
    # a mode entered by a failing command is left again, and reported as such
    tos = [e["to"] for e in tool.events_named("mode", since=m)]
    assert len(tos) % 2 == 0 and all(t == "none" for t in tos[1::2]), tos

    tool.cmd("broadcast budget")
    assert tool.cmd("mode")["mode"] == "broadcast"


def test_remaining_commands(tool):
    """Commands no other test runs (protocol conformance report): their replies match the spec."""
    dev = tool.cmd("bonding create Remote")["id"]
    assert tool.cmd("bonding io Remote yesno")["io"] == "yesno"
    rows = [d for d in tool.cmd("bonding list").data if d["id"] == dev]
    assert rows[0]["io"] == "yesno" and rows[0]["source"] == "0"
    assert rows[0]["connection"] == "disconnected" and rows[0]["bond"] == "absent"
    show = tool.cmd(f"bonding show {dev} --verbose")
    assert show["name"] == "Remote" and show["rssi"] == "127" and show["phy"] == "none"
    tool.cmd("le-source attach Remote")
    assert tool.cmd("le-source status")["identity"] == "Remote"
    assert tool.cmd("le-source detach")["id"] == dev

    tool.cmd("evt off source_state")
    assert "source_state" in tool.cmd("evt list")["off"]
    tool.cmd("evt on source_state")

    tool.cmd("mode unicast")
    assert tool.cmd("le-source stop")["queued"] == "1"


def test_connect_timeout(tool):
    """A connection to an absent device ends with bonding_connect_failed after the timeout."""
    dev = tool.cmd("bonding create")["id"]
    tool.cmd(f"bonding connect {dev} address=C0:11:22:33:44:55/random timeout=500")
    ev = tool.wait_evt("bonding_connect_failed", id=dev, timeout=10)
    assert ev["peer"] == "C0:11:22:33:44:55/random"
    row = next(d for d in tool.cmd("bonding list").data if d["id"] == dev)
    assert row["connection"] == "disconnected"


def test_identity_slots_reused(tool):
    """Deleting the newest, an older or every device always leaves room for new ones (deleting
    the last identity shrank Zephyr's table, and the slot could never be used again)."""
    for _ in range(3):
        a = tool.cmd("bonding create a")["id"]
        tool.cmd(f"bonding delete {a}")  # the newest
    a = tool.cmd("bonding create a")["id"]
    b = tool.cmd("bonding create b")["id"]
    tool.cmd(f"bonding delete {a}")  # an older one
    c = tool.cmd("bonding create c")["id"]
    assert c == a
    for d in (b, c):
        tool.cmd(f"bonding delete {d}")
    ids = [tool.cmd(f"bonding create n{i}")["id"] for i in range(4)]
    assert len(set(ids)) == 4 and tool.cmd("bonding list")["count"] == "4"
