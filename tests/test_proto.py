# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""CLI protocol: keys checked by the firmware match protocol/hatool.yaml, JSON Lines and human
modes, quoting, event filter, limits / id / time. Runs on the simulation or a real DK."""

import json
import pathlib
import time

import yaml

from helpers import raw_lines

ROOT = pathlib.Path(__file__).resolve().parent.parent
SPEC = yaml.safe_load((ROOT / "protocol/hatool.yaml").read_text())


def test_firmware_matches_spec(tool):
    """Every command of the firmware with exactly the keys of the spec, and the other way round."""
    fw = {
        d["cmd"]: set() if d["keys"] == "none" else set(d["keys"].split(","))
        for d in tool.cmd("cli spec").data
    }
    spec = {
        c.get("wire", c["cmd"]): {k["name"] for k in c.get("keys", [])} for c in SPEC["commands"]
    }
    assert set(fw) == set(spec), set(fw) ^ set(spec)
    for cmd, keys in spec.items():
        assert fw[cmd] == keys, cmd
    assert tool.cmd("version").int("proto") == SPEC["protocol"]


def test_unknown_key_rejected(tool):
    rep = tool.cmd("bonding create frq=1", check=False)
    assert rep.errno == -22 and "key=frq" in rep.raw and "allowed=addr,io" in rep.raw
    rep = tool.cmd("broadcast 1 bis tone 1 frequency=500", check=False)
    assert rep.errno == -22 and "key=frequency" in rep.raw
    assert tool.cmd("status --verbose")["identities"] == "0"  # nothing was created


def test_json_lines(tool):
    tool.cmd("reset")
    raw_lines(tool, "proto json")
    try:
        # Non-ASCII goes as \xHH escapes (the shell takes ASCII only; the clients do this)
        lines = raw_lines(tool, 'bonding create "Hall \\"A\\" caf\\xc3\\xa9"')
        lines += raw_lines(tool, "bonding list")
        lines += raw_lines(tool, "bonding create bad=1")
        objs = [json.loads(ln) for ln in lines]  # every line is JSON
        ok = [o for o in objs if o["type"] == "ok"]
        assert ok[0]["name"] == 'Hall "A" café'
        data = [o for o in objs if o["type"] == "data"]
        assert data[0]["tag"] == "IDENTITY" and data[0]["name"] == 'Hall "A" café'
        err = next(o for o in objs if o["type"] == "err")
        assert err["errno"] == -22 and err["key"] == "bad"
        evt = next(o for o in objs if o["type"] == "evt")
        assert evt["evt"] == "mode" and isinstance(evt["t"], int)
    finally:
        tool.cmd("proto text")
    # Text mode quotes the same value
    assert tool.cmd("bonding list").data[0]["name"] == 'Hall "A" café'


def test_human_mode(tool):
    lines = raw_lines(tool, "proto human")
    lines += raw_lines(tool, "status")
    try:
        assert any(ln.startswith("ha> ") or ln == "status" or "status" in ln for ln in lines)
        assert any(ln.startswith("OK mode=") for ln in lines)
    finally:
        tool.cmd("proto text")
    assert tool.cmd("version")["proto"] == "1"  # clean text again


def test_limits_id_time(tool):
    lim = tool.cmd("limits")
    assert lim.int("bigs") == 3 and lim.int("bis_total") == 16
    assert len([d for d in lim.data if d["_tag"] == "LC3"]) == 16
    chip = tool.cmd("id")["id"]
    assert len(chip) >= 8 and tool.cmd("id")["id"] == chip
    t1 = tool.cmd("time").int("us")
    time.sleep(0.2)
    assert tool.cmd("time").int("us") - t1 >= 150_000


def test_event_filter(tool):
    tool.cmd("reset")
    assert tool.cmd("evt off mode bonding_connected")["off"] == "mode,bonding_connected"
    m = tool.mark()
    tool.cmd("bonding create")
    time.sleep(0.3)
    assert not tool.events_named("mode", since=m)
    assert tool.cmd("evt on mode")["off"] == "bonding_connected"
    tool.cmd("reset")
    tool.wait_evt("mode", to="none", timeout=5)
    assert tool.cmd("evt on")["off"] == "none"


def test_non_ascii_line_refused(tool):
    """Raw non-ASCII bytes are refused with a shell error instead of being dropped silently
    (Zephyr patch 0007); escaped in quotes they are accepted."""
    for line in ("é", "broadcast 1 create name=Zażółć bis=1"):
        rep = tool.cmd(line, check=False)
        assert not rep.ok and "non-ASCII" in rep.raw, rep.raw
    assert tool.cmd("mode")["mode"] == "none"  # nothing ran
    tool.cmd('broadcast 1 create name="Za\\xc5\\xbc\\xc3\\xb3\\xc5\\x82\\xc4\\x87" bis=1')


def test_every_shell_error_is_a_reply(tool):
    """Each error the Zephyr shell prints itself is recognised as the final reply (ERR -22),
    so no client waits for a timeout."""
    for line in ('"unterminated', "x " * 17, "nosuchcmd", "broadcast", "mode a b c d e"):
        rep = tool.cmd(line, check=False, timeout=3)
        assert not rep.ok and rep.errno == -22, (line, rep.raw)

    # unknown word and more words than CONFIG_SHELL_ARGC_MAX: one reply (Zephyr patch 0009)
    lines = raw_lines(tool, "nosuchcmd " * 30)
    assert len([ln for ln in lines if "not found" in ln or "Too many" in ln]) == 1, lines


def test_duplicate_key_refused(tool):
    """A key given twice is an error instead of silently taking the first value."""
    rep = tool.cmd("broadcast budget preset=16_2 preset=48_2", check=False)
    assert rep.errno == -22 and "duplicate key key=preset" in rep.raw, rep.raw
    assert tool.cmd("mode")["mode"] == "none"


def test_help_from_spec(tool):
    """'help' lists commands from the specification: all, a group, or one command."""
    everything = tool.cmd("help")
    assert everything.int("commands") == len(SPEC["commands"])
    group = tool.cmd("help bonding")
    paths = [d["cmd"] for d in group.data]
    assert "bonding connect" in paths and all(p.startswith("bonding ") for p in paths)
    one = tool.cmd("help bonding pair")
    assert [d["cmd"] for d in one.data] == ["bonding pair"] and "sec=" in one.data[0]["usage"]
    rep = tool.cmd("help nothing here", check=False)
    assert not rep.ok and rep.errno == -22


def test_unknown_command_hints(tool):
    """A mistyped word gets the nearest commands as hint=, every word of a group otherwise."""
    rep = tool.cmd("bonding conect 1", check=False)
    assert rep.errno == -22 and "unknown command" in rep.raw
    assert "bonding connect" in rep.values["hint"]
    # docs/broadcast-ux.md: le-source volume increase -> up, down, set
    rep = tool.cmd("le-source volume increase", check=False)
    hints = {h.split(" <")[0] for h in rep.values["hint"].split("; ")}
    assert hints >= {"le-source volume up", "le-source volume down", "le-source volume set"}
    rep = tool.cmd("le-source status extra", check=False)
    assert rep.errno == -22 and "le-source status" in rep.values["hint"]
    rep = tool.cmd("bonding pair", check=False)
    assert rep.errno == -22 and "wrong parameter count" in rep.raw and "usage=" in rep.raw
    rep = tool.cmd("bonding list --bogus", check=False)
    assert rep.errno == -22 and "unknown flag" in rep.raw


def test_hint_in_human_mode(tool):
    """Human mode prints the hint on lines of its own after 'Try:'."""
    raw_lines(tool, "proto human")
    try:
        lines = raw_lines(tool, "bonding conect 1")
        i = next(i for i, ln in enumerate(lines) if ln.startswith("ERR -22"))
        assert "hint=" not in lines[i]
        assert lines[i + 1] == "Try:" and lines[i + 2].strip().startswith("bonding connect")
    finally:
        tool.cmd("proto text")
