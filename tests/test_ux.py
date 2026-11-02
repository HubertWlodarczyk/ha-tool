# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Acceptance criteria of docs/broadcast-ux.md: identities by name, connect by address or
Bluetooth name, one unicast session for bonding and le-source, errors that name the next step,
layered status, stop and reset."""

import pytest

from helpers import STREAM_TIMEOUT, near, raw_lines

DUT_NAME = "DUT-HA"  # CONFIG_BT_DEVICE_NAME of dut_sim


def test_identities_by_name(tool):
    """A name is the tester's handle: unique, never a number; every command takes it or the id."""
    t = tool.cmd("bonding create Phone io=yesno")
    assert t["name"] == "Phone" and t["io"] == "yesno"
    rep = tool.cmd("bonding create Phone", check=False)
    assert rep.errno == -17 and "bonding delete" in rep.values["hint"]  # -EEXIST
    assert tool.cmd("bonding create 42", check=False).errno == -22
    assert tool.cmd("bonding show Phone")["id"] == t["id"]
    assert tool.cmd(f"bonding show {t['id']}")["name"] == "Phone"
    rep = tool.cmd("bonding show Tablet", check=False)
    assert rep.errno == -2 and rep.values["hint"] == "bonding list"
    assert tool.cmd("bonding io Phone keyboard")["io"] == "keyboard"
    assert tool.cmd("bonding create")["name"].startswith("identity")  # a default name
    tool.cmd("bonding delete Phone")
    assert tool.cmd("bonding list")["count"] == "1"


def test_connect_by_bluetooth_name(setup):
    """bonding scan lists named devices; connecting by name finds exactly one and reports both
    the name and the address."""
    ha, _, addr = setup
    found = ha.cmd("bonding scan timeout=1500")
    assert {"addr": addr, "name": DUT_NAME} in [
        {"addr": d["addr"], "name": d["name"]} for d in found.data
    ]
    ha.cmd("bonding create Phone")
    rep = ha.cmd(f"bonding connect Phone bluetooth-name={DUT_NAME}")
    assert rep["device"] == addr and rep["device_name"] == DUT_NAME
    ha.wait_evt("bonding_connected", timeout=20)
    show = ha.cmd("bonding show Phone")
    assert show["connection"] == "connected" and show["device_name"] == DUT_NAME
    assert show["security"] == "unencrypted" and show["bond"] == "absent"  # no pairing yet

    ha.cmd("bonding create Tablet")
    rep = ha.cmd("bonding connect Tablet bluetooth-name=Nobody scan=500", check=False)
    assert rep.errno == -2 and rep.values["hint"] == "bonding scan"
    rep = ha.cmd("bonding connect Tablet", check=False)  # neither address= nor bluetooth-name=
    assert rep.errno == -22


@pytest.fixture
def two_aids(sim):
    """ha_tool and two hearing aids that advertise the same Bluetooth name."""
    ha, a, b = sim.start([("ha_tool", "ha_tool"), ("dut", "dut_sim"), ("dut2", "dut_sim")])
    return ha, a.cmd("addr")["addr"], b.cmd("addr")["addr"]


def test_ambiguous_bluetooth_name(two_aids):
    """Two devices answer to the name: both are listed, none is chosen, nothing connects."""
    ha, a, b = two_aids
    ha.cmd("bonding create Phone")
    rep = ha.cmd(f"bonding connect Phone bluetooth-name={DUT_NAME}", check=False)
    assert rep.errno == -17 and "connection not started" in rep.raw
    assert {d["addr"] for d in rep.data} == {a, b}
    assert rep.values["hint"].startswith("bonding connect Phone address=")
    assert ha.cmd("bonding show Phone")["connection"] == "disconnected"

    # the address given by the hint connects
    ha.cmd(rep.values["hint"])
    ha.wait_evt("bonding_connected", timeout=20)


def test_unicast_session(setup):
    """The flow of the design document: bonding prepares the identity, le-source uses it."""
    ha, dut, addr = setup
    ha.cmd("mode unicast")
    ha.cmd("bonding create Phone")
    ha.cmd(f"bonding connect Phone address={addr}")
    ha.wait_evt("bonding_connected", timeout=20)
    m = ha.mark()
    ha.cmd("bonding pair Phone sec=2")
    ha.wait_evt("bonding_paired", since=m, bonded=1, timeout=20)
    ha.cmd("le-source attach Phone")

    st = ha.cmd("status")
    assert st["mode"] == "unicast" and st["identity"] == "Phone" and st["device"] == addr
    assert st["connection"] == "connected" and st["security"] == "encrypted"
    assert st["bond"] == "present" and st["source"] == "idle"

    m = dut.mark()
    ha.cmd("le-source music start freq=440")
    dut.wait_evt("dut_audio", since=m, src="unicast", freq=near(440), timeout=STREAM_TIMEOUT)
    src = ha.cmd("le-source status")
    assert src["identity"] == "Phone" and src["stream"] == "active" and src["state"] == "music"

    # one VCP operation at a time: each waits for the hearing aid's new state
    ha.cmd("le-source volume set 120")
    ha.wait_evt("source_volume", volume=120, timeout=15)
    m = ha.mark()
    ha.cmd("le-source volume up")
    up = int(ha.wait_evt("source_volume", since=m, timeout=15)["volume"])
    assert up > 120
    m = ha.mark()
    ha.cmd("le-source volume down")
    assert int(ha.wait_evt("source_volume", since=m, timeout=15)["volume"]) < up
    ha.cmd("le-source preset set 2")
    ha.wait_evt("source_preset", active=2, timeout=15)
    m = dut.mark()
    ha.cmd("le-source preset prev")
    dut.wait_evt("dut_preset", since=m, active=1, timeout=15)
    ha.cmd("le-source music stop")

    # stop: links down, identity and bond kept; nothing else changes
    m = ha.mark()
    assert ha.cmd("stop")["mode"] == "unicast"
    ha.wait_evt("bonding_disconnected", since=m, timeout=20)
    row = ha.cmd("bonding list").data[0]
    assert row["name"] == "Phone" and row["connection"] == "disconnected"
    assert row["bond"] == "present" and row["source"] == "1"


def test_errors_name_the_next_step(setup):
    """Source and pairing errors say what is missing and which command fixes it."""
    ha, _, _ = setup
    rep = ha.cmd("le-source music start", check=False)
    assert rep.errno == -107 and rep.values["hint"] == "le-source attach <identity>"
    ha.cmd("bonding create Phone")
    ha.cmd("bonding create Tablet")
    ha.cmd("le-source attach Phone")
    rep = ha.cmd("le-source volume up", check=False)
    assert rep.errno == -107 and rep.values["hint"].startswith("bonding connect Phone")
    rep = ha.cmd("le-source attach Tablet", check=False)
    assert rep.errno == -16 and rep.values["hint"] == "le-source detach"
    rep = ha.cmd("bonding pair Phone", check=False)
    assert rep.errno == -107 and rep.values["hint"].startswith("bonding connect Phone")
    ha.cmd("le-source detach")
    assert ha.cmd("le-source attach Tablet")["name"] == "Tablet"
    rep = ha.cmd("broadcast 1 create", check=False)
    assert rep.errno == -16 and rep.values["hint"] == "mode broadcast"


def test_reset_needs_yes_in_human_mode(tool):
    """Interactive (human) reset is confirmed with --yes; scripts (text, json) need no flag."""
    tool.cmd("bonding create Phone")
    raw_lines(tool, "proto human")
    try:
        lines = raw_lines(tool, "reset")
        i = next(i for i, ln in enumerate(lines) if ln.startswith("ERR -1"))
        assert lines[i + 1] == "Try:" and lines[i + 2].strip() == "reset --yes"
        lines = raw_lines(tool, "reset --yes", settle=3)
        assert any(ln.startswith("OK") for ln in lines)
    finally:
        tool.cmd("proto text")
    assert tool.cmd("mode")["mode"] == "none" and tool.cmd("bonding list")["count"] == "0"
    tool.cmd("bonding create Phone")
    tool.cmd("reset")  # text: no confirmation
    assert tool.cmd("bonding list")["count"] == "0"


# --- broadcast ---


def test_two_independent_bigs(pair):
    """Two BIGs built without any selection command, different subgroups, languages,
    contexts, locations and tones, started and stopped independently; the states follow."""
    ha, dut = pair
    ha.cmd("mode broadcast")
    assert ha.cmd("broadcast 1 create name=Concert id=0x123456 phy=2m rtn=1")["state"] == "draft"
    ha.cmd('broadcast 1 subgroup add preset=48_2 lang=pol ctx=media program="Music"')
    assert ha.cmd("broadcast 1 show")["state"] == "draft"  # no BIS yet
    ha.cmd("broadcast 1 bis add subgroup=1 location=left freq=440")
    ha.cmd("broadcast 1 bis add subgroup=1 location=right freq=880")
    ha.cmd("broadcast 2 create name=Announcements id=0x234567 phy=1m interval=10 rtn=1")
    ha.cmd('broadcast 2 subgroup add preset=16_2 lang=pol ctx=instructional program="Komunikaty"')
    ha.cmd("broadcast 2 bis add subgroup=1 location=mono freq=1000")

    listing = ha.cmd("broadcast list")
    assert [(d["big"], d["name"], d["state"]) for d in listing.data] == [
        ("1", "Concert", "ready"),
        ("2", "Announcements", "ready"),
    ]
    subgroups = ha.cmd("broadcast 1 subgroup list").data
    assert subgroups[0]["ctx"] == "media" and subgroups[0]["bis"] == "2"
    bises = ha.cmd("broadcast 1 bis list").data
    assert [b["location"] for b in bises] == ["left", "right"]

    ha.cmd("broadcast 2 start")
    ha.wait_evt("broadcast_started", big=2, timeout=15)
    assert ha.cmd("broadcast 1 show")["state"] == "ready"  # untouched
    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", big=1, timeout=15)
    m = dut.mark()
    dut.cmd("bcast scan name=Announcements")
    dut.wait_evt("dut_audio", since=m, src="bcast_bis1", freq=near(1000), timeout=30)
    dut.cmd("bcast stop")

    ha.cmd("broadcast 2 stop")
    ha.wait_evt("broadcast_stopped", big=2, timeout=15)
    states = {d["big"]: d["state"] for d in ha.cmd("broadcast list").data}
    assert states == {"1": "active", "2": "stopped"}


def test_configuration_changes(pair):
    """set / delete of subgroups and BISes, renumbering, and what may change while active."""
    ha, _ = pair
    ha.cmd("broadcast 1 create name=Edit")
    ha.cmd("broadcast 1 subgroup add preset=16_2 lang=pol")
    ha.cmd("broadcast 1 subgroup add preset=24_2 lang=eng")
    for f in (300, 400, 500):
        ha.cmd(f"broadcast 1 bis add subgroup=1 freq={f}")
    ha.cmd("broadcast 1 bis add subgroup=2 location=right freq=600")
    rep = ha.cmd("broadcast 1 bis add subgroup=3", check=False)
    assert rep.errno == -22 and rep.values["hint"] == "broadcast 1 subgroup list"

    # BIS numbers stay contiguous after a delete
    ha.cmd("broadcast 1 bis delete 2")
    assert [b["tone"] for b in ha.cmd("broadcast 1 bis list").data] == ["300", "500", "600"]
    assert ha.cmd("broadcast 1 bis set 3 location=left freq=700")["location"] == "left"

    # a subgroup with BISes is kept; empty, it goes and the later ones move down
    rep = ha.cmd("broadcast 1 subgroup delete 1", check=False)
    assert rep.errno == -16 and rep.values["hint"].startswith("broadcast 1 bis delete")
    ha.cmd("broadcast 1 bis delete 1")
    ha.cmd("broadcast 1 bis delete 1")
    assert ha.cmd("broadcast 1 subgroup delete 1")["subgroups"] == "1"
    (sg,) = ha.cmd("broadcast 1 subgroup list").data
    assert sg["lang"] == "eng" and sg["bis"] == "1"
    assert ha.cmd("broadcast 1 bis list").data[0]["sg"] == "1"

    # active: the structure is locked, tones and the metadata of one subgroup change live
    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", big=1, timeout=15)
    for line in ("broadcast 1 bis add", "broadcast 1 set rtn=1", "broadcast 1 bis set 1 rate=48"):
        rep = ha.cmd(line, check=False)
        assert rep.errno == -16 and rep.values["hint"] == "broadcast 1 stop", line
    assert ha.cmd("broadcast 1 bis tone 1 freq=1000")["freq"] == "1000"
    assert ha.cmd("broadcast 1 subgroup set 1 program=News")["program"] == "News"
    ha.cmd("broadcast 1 restart")
    ha.wait_evt("broadcast_started", big=1, timeout=15)


def test_bulk_operations_report_each_big(pair):
    """start / stop / restart --all: one RESULT per BIG; a partial success is an ERR."""
    ha, _ = pair
    ha.cmd("broadcast 1 create name=One bis=1")
    ha.cmd("broadcast 2 create name=Two bis=2")
    ha.cmd("broadcast 3 create name=Heavy preset=48_6 bis=8 rtn=3")  # over the budget
    rep = ha.cmd("broadcast start", check=False)
    assert rep.errno == -22 and "--all" in rep.values["hint"]

    rep = ha.cmd("broadcast start --all", check=False)
    results = {d["big"]: d["result"] for d in rep.data}
    assert results == {"1": "started", "2": "started", "3": "failed"}
    assert rep.errno == -7 and "2 of 3" in rep.raw  # -E2BIG of BIG 3
    heavy = next(d for d in rep.data if d["big"] == "3")
    assert "fit one event of the controller" in heavy["reason"]
    assert ha.cmd("broadcast 3 show")["state"] == "error"
    pct = ha.cmd("broadcast budget")
    assert float(pct["used_pct"]) > 0 and len(pct.data) == 2

    ha.cmd("broadcast 3 delete")
    rep = ha.cmd("broadcast start --all")  # both already active: nothing to do, no failure
    assert rep["started"] == "0" and {d["result"] for d in rep.data} == {"active"}
    assert ha.cmd("broadcast restart --all")["restarted"] == "2"
    rep = ha.cmd("broadcast stop --all")
    assert rep["stopped"] == "2" and {d["result"] for d in rep.data} == {"stopped"}
    ha.cmd("broadcast 1 start")
    rep = ha.cmd("broadcast stop --all")  # only what was running counts as stopped
    assert rep["count"] == "2" and rep["stopped"] == "1"
    assert sorted(d["result"] for d in rep.data) == ["inactive", "stopped"]


def test_several_failures_are_all_named(pair):
    """start --all with two failing BIGs names both, not only the first."""
    ha, _ = pair
    ha.cmd("broadcast 1 create name=Ok bis=1")
    ha.cmd("broadcast 2 create name=Empty")
    ha.cmd("broadcast 3 create name=Empty2")
    rep = ha.cmd("broadcast start --all", check=False)
    assert rep.errno == -22 and "1 of 3 BIGs started, BIGs 2, 3 failed" in rep.raw


def test_broadcast_errors_name_the_next_step(pair):
    """Incomplete or existing BIGs, unknown numbers: the error says what to run."""
    ha, _ = pair
    rep = ha.cmd("broadcast 1 show", check=False)
    assert rep.errno == -2 and rep.values["hint"] == "broadcast 1 create"
    ha.cmd("broadcast 2 create name=Empty")
    rep = ha.cmd("broadcast 2 create", check=False)
    assert rep.errno == -17 and rep.values["hint"] == "broadcast 2 delete"
    rep = ha.cmd("broadcast 2 start", check=False)
    assert "no subgroup" in rep.raw and rep.values["hint"].startswith("broadcast 2 subgroup add")
    ha.cmd("broadcast 2 subgroup add")
    rep = ha.cmd("broadcast 2 start", check=False)
    assert "no BIS" in rep.raw
    assert rep.values["hint"] == "broadcast 2 bis add subgroup=1 location=mono"
    assert ha.cmd("broadcast 9 show", check=False).errno == -22
    rep = ha.cmd("broadcast 2 bis tone 5 freq=1", check=False)
    assert rep.errno == -2 and rep.values["hint"] == "broadcast 2 bis list"


def test_human_views(pair):
    """proto human: broadcast list is a table, show is a tree of subgroups and BISes."""
    ha, _ = pair
    ha.cmd("broadcast 1 create name=Tree preset=24_2 bis=2 lang=pol")
    raw_lines(ha, "proto human")
    try:
        table = raw_lines(ha, "broadcast list", settle=2)
        assert any(ln.split()[:3] == ["BIG", "NAME", "STATE"] for ln in table)
        assert any(ln.split()[:3] == ["1", "Tree", "ready"] for ln in table)
        tree = raw_lines(ha, "broadcast 1 show", settle=2)
        assert any("Subgroup 1: 24_2, media, language pol" in ln for ln in tree)
        assert any("BIS 2: right" in ln for ln in tree)
    finally:
        ha.cmd("proto text")


def test_human_events_are_sentences(setup):
    """proto human describes the main events in words (their raw EVT only with --verbose);
    text keeps the EVT lines."""
    ha, _, addr = setup
    ha.cmd("bonding create Phone")
    raw_lines(ha, "proto human")
    try:
        lines = raw_lines(ha, f"bonding connect Phone address={addr}", settle=4)
        # (the first line after the reply may carry the shell prompt)
        assert any(f"Connection established: Phone with {addr}" in ln for ln in lines)
        # described events replace their EVT line; the others stay raw
        assert not any("bonding_connected" in ln for ln in lines)
        raw_lines(ha, "proto human --verbose")
        lines = raw_lines(ha, "bonding pair Phone", settle=5)
        assert any("Pairing completed, bond stored for Phone." in ln for ln in lines)
        assert any(ln.startswith("EVT") and "bonding_paired" in ln for ln in lines)
    finally:
        ha.cmd("proto text")
    assert ha.events_named("bonding_paired")  # the text protocol still sees the events


def test_help_and_hints_guide_the_typing(tool):
    """help finds a command without its BIG number, a swapped word order gets the right line,
    hints show targets as <big>, and an impossible request is refused at once."""
    assert tool.cmd("help broadcast bis add")["commands"] == "1"
    tool.cmd("mode unicast")
    tool.cmd("bonding create Phone")
    rep = tool.cmd("bonding Phone show", check=False)
    assert rep.errno == -22 and rep.values["hint"] == "bonding show Phone"
    rep = tool.cmd("broadcast 1 bis ad", check=False)
    assert "{big}" not in rep.raw
    tool.cmd("le-source attach Phone")
    rep = tool.cmd("le-source call end", check=False)  # no call
    assert rep.errno == -1 and rep.values["hint"] == "le-source status"


def test_automatic_names_stay_unique(tool):
    """A default name never collides with one the tester chose."""
    tool.cmd("mode unicast")
    first = tool.cmd("bonding create identity2")  # takes the name the next default would get
    second = tool.cmd("bonding create")
    assert first["id"] == "1" and second["id"] == "2"
    assert second["name"] == "identity2-2"


def test_authenticated_pairing_needs_io(setup):
    """sec=3/4 with io=none cannot work: the error says why and what to change."""
    ha, _, addr = setup
    ha.cmd("mode unicast")
    ha.cmd("bonding create Phone")
    ha.cmd(f"bonding connect Phone address={addr}")
    ha.wait_evt("bonding_connected", timeout=20)
    rep = ha.cmd("bonding pair Phone sec=4", check=False)
    assert rep.errno == -22 and "io=none" in rep.raw
    assert rep.values["hint"] == "bonding pair Phone sec=2; bonding io Phone kbd_display"


def test_human_empty_lists_and_status(tool):
    """proto human: an empty list says how to start, status is shown layer by layer."""
    raw_lines(tool, "proto human")
    try:
        lines = raw_lines(tool, "bonding list", settle=2)
        assert any("bonding create <name>" in ln for ln in lines)
        lines = raw_lines(tool, "broadcast list", settle=2)
        assert any("broadcast 1 create" in ln for ln in lines)
        status = raw_lines(tool, "status", settle=2)
        assert any(ln.startswith("Mode:") for ln in status)
        assert any("mode unicast | mode broadcast" in ln for ln in status)
    finally:
        tool.cmd("proto text")


def test_human_sentences_of_a_whole_session(setup):
    """proto human over a whole session: every kind of event a tester follows reads as a
    sentence (pairing, services, stream, volume, presets, microphone, balance, call, alert,
    disconnection, broadcast)."""
    ha, _, addr = setup
    ha.cmd("mode unicast")
    ha.cmd("bonding create Phone")
    raw_lines(ha, "proto human")
    seen = []
    try:
        steps = [
            (f"bonding connect Phone address={addr}", 4),
            ("bonding pair Phone", 5),
            ("le-source attach Phone", 1),
            ("le-source music start preset=48_2 freq=440", 8),
            ("le-source volume set 128", 3),
            ("le-source preset next", 3),
            ("le-source preset list", 3),
            ("le-source mic mute on", 3),
            ("le-source mic gain 5", 3),
            ("le-source balance set 10", 3),
            ("bonding phy Phone 1m", 3),
            ("bonding param Phone interval=50", 4),
            ("le-source call incoming name=John", 5),
            ("le-source call accept", 6),
            ("le-source call end", 6),
            ("le-source alert ms=1000 freq=1500", 5),
            ("bonding disconnect Phone", 4),
        ]
        for line, settle in steps:
            seen += raw_lines(ha, line, settle=settle)
        seen += raw_lines(ha, "mode broadcast", settle=3)
        seen += raw_lines(ha, "broadcast 1 create name=Hall bis=1", settle=1)
        seen += raw_lines(ha, "broadcast 1 start", settle=4)
        seen += raw_lines(ha, "broadcast 1 stop", settle=3)
    finally:
        ha.cmd("proto text")

    text = "\n".join(seen)
    for sentence in (
        "Connection established: Phone",
        "Pairing completed",
        "Services of Phone found: audio sink",
        "Audio stream active: media, 48_2",
        "Device volume 128/255 (50%)",
        "Active preset",
        "Microphone muted",
        "Balance offset 10",
        "PHY of Phone",
        "Connection parameters of Phone",
        "Call 1 incoming",
        "Audio stream now ringtone",
        "Call 1 ended",
        "Phone disconnected",
        "Mode: broadcast.",
        'BIG 1 "Hall" active, 1 BIS.',
        "BIG 1 stopped.",
    ):
        assert sentence in text, (sentence, text[-3000:])
    # low-level events stay out of the non-verbose view
    assert "bonding_mtu" not in text and "source_stream_started" not in text
