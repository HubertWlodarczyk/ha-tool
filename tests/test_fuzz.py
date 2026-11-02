# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""CLI fuzzing on the live simulation: thousands of command lines generated from the protocol
spec (protocol/hatool.yaml), valid and broken, in text and JSON mode.

Invariants checked for every line:
  - exactly one final reply (OK / ERR / shell error), nothing left unanswered
  - in JSON mode every reply and event line is valid JSON
  - no fatal error event and no controller assertion
At the end the board still answers, 'status' is consistent and no output line was cut.

Deterministic (seeded); slow, run with: scripts/test.sh -m slow test_fuzz.py
Environment: HA_FUZZ_LINES (default 2500), HA_FUZZ_SEED (default 1).
"""

from __future__ import annotations

import json
import os
import pathlib
import random
import time

import pytest
import yaml

from simlib import ROOT
from simlib.device import SHELL_ERR_RE as SHELL_ERR

pytestmark = pytest.mark.slow

SPEC = yaml.safe_load((ROOT / "protocol/hatool.yaml").read_text())
LINES = int(os.environ.get("HA_FUZZ_LINES", "2500"))
SEED = int(os.environ.get("HA_FUZZ_SEED", "1"))
# Commands that change the protocol or restart the board are driven by the fuzzer itself
DRIVEN = {"proto"}
REPLY_TIMEOUT = 15.0

INTS = [
    "0",
    "1",
    "2",
    "3",
    "7",
    "8",
    "15",
    "16",
    "-1",
    "255",
    "256",
    "1000",
    "65535",
    "65536",
    "2147483647",
    "2147483648",
    "-2147483649",
    "0x10",
    "1e3",
    "",
    "x",
    "1.5",
    "+1",
    "007",
]
STRS = [
    "a",
    "Hearing Test",
    "x" * 31,
    "x" * 32,
    "y" * 200,
    'quo"te',
    "back\\slash",
    "eq=ual",
    "zażółć",
    "\\x41\\x42",
    "tab\there",
    "",
    " lead",
    "☃",
]
ADDRS = [
    "C0:11:22:33:44:55/random",
    "00:11:22:33:44:55/public",
    "C0:11:22:33:44:55",
    "zz:11:22:33:44:55/random",
    "C0:11:22:33:44/random",
    "FF:FF:FF:FF:FF:FF/random",
]
GARBAGE = [
    '"unterminated',
    'broadcast 1 create name="open',
    "\\",
    "\\x",
    "\\xZZ",
    "=",
    "==",
    "a=b=c",
    "dev   create   name=a",
    "ab\x7f\x7fversion",
    "é",
    "☃ ☃",
    "bcast " * 60,
    "x" * 300,
    "bonding create " + "k=v " * 20,
]


def value(rng: random.Random, kind: str, values: list | None = None) -> str:
    if kind == "enum" and values and rng.random() < 0.8:
        return str(rng.choice(values))
    if kind in ("int", "hex"):
        return rng.choice(INTS)
    if kind == "bool":
        return rng.choice(["0", "1", "2", "yes", ""])
    if kind == "float":
        return rng.choice(["0", "1.5", "-3.25", "nan", "1e9", "x"])
    if kind == "addr":
        return rng.choice(ADDRS)
    return rng.choice(STRS + INTS)


def quote(s: str) -> str:
    if s and not any(c in s for c in ' "\\'):
        return s
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def command_line(rng: random.Random, cmd: dict) -> str:
    """Words of the command line (targets such as {big} filled in), then the other arguments."""
    args = {a["name"]: a for a in cmd.get("args", [])}
    words = []
    for w in cmd.get("wire", cmd["cmd"]).split():
        if w.startswith("{"):
            a = args.pop(w[1:-1])
            words.append(quote(value(rng, a.get("type", "str"), a.get("values"))))
        else:
            words.append(w)
    for a in args.values():
        r = rng.random()
        if r < 0.08:
            continue  # missing argument
        words.append(quote(value(rng, a.get("type", "str"), a.get("values"))))
    keys = cmd.get("keys", [])
    for k in rng.sample(keys, rng.randint(0, len(keys))) if keys else []:
        words.append(f"{k['name']}={quote(value(rng, k.get('type', 'str'), k.get('values')))}")
    for f in cmd.get("flags", []):
        if f.get("required") or rng.random() < 0.5:
            words.append(f"--{f['name']}")
    r = rng.random()
    if r < 0.07:
        words.append(f"bogus{rng.randint(0, 9)}=1")  # unknown key
    elif r < 0.12:
        words.append(rng.choice(["extra", "1", "x=1", '""']))  # extra argument
    elif r < 0.15 and keys:
        k = rng.choice(keys)["name"]
        words += [f"{k}=1", f"{k}=2"]  # duplicate key
    return " ".join(words)


def generate(rng: random.Random):
    """(kind, line): kind 'cmd' expects one reply; 'proto' switches the protocol."""
    commands = [c for c in SPEC["commands"] if c["cmd"].split()[0] not in DRIVEN]
    for _ in range(LINES):
        r = rng.random()
        if r < 0.02:
            yield "proto", rng.choice(["proto json", "proto text"])
        elif r < 0.03:
            yield "cmd", "reset"
        elif r < 0.05:
            yield "cmd", f"mode {rng.choice(['none', 'unicast', 'broadcast'])}"
        elif r < 0.12:
            yield "cmd", rng.choice(GARBAGE)
        else:
            yield "cmd", command_line(rng, rng.choice(commands))


def final_reply(line: str, json_mode: bool) -> bool:
    if SHELL_ERR.search(line):
        return True
    if json_mode and line.startswith("{"):
        obj = json.loads(line)  # invariant: valid JSON (raises otherwise)
        return obj.get("type") in ("ok", "err")
    return line.startswith(("OK", "ERR"))


def test_cli_fuzz(tool, request):
    rng = random.Random(SEED)
    json_mode = False
    log = pathlib.Path(request.node.name + ".lines")
    sent: list[str] = []
    stats = {"lines": 0, "ok": 0, "err": 0, "shell": 0}

    def wait_reply(line: str, start: int) -> int:
        """Index after the final reply of `line`; checks every line on the way."""
        deadline = time.monotonic() + REPLY_TIMEOUT
        idx = start
        with tool._cond:
            while True:
                while idx < len(tool.lines):
                    ln = tool.lines[idx]
                    idx += 1
                    assert "ASSERTION" not in ln and 'evt="fatal"' not in ln, (line, ln)
                    assert not ln.startswith("EVT") or " fatal " not in ln, (line, ln)
                    if json_mode and ln.startswith("{"):
                        json.loads(ln)
                    if final_reply(ln, json_mode):
                        kind = (
                            "shell"
                            if SHELL_ERR.search(ln)
                            else ("ok" if '"ok"' in ln or ln.startswith("OK") else "err")
                        )
                        stats[kind] += 1
                        return idx
                remaining = deadline - time.monotonic()
                assert remaining > 0, f"no reply to {line!r} (after {sent[-5:]})"
                tool._cond.wait(remaining)

    def no_extra_reply(start: int, line: str) -> None:
        """No second final reply arrived for the previous line."""
        with tool._cond:
            extra = [ln for ln in tool.lines[start:] if final_reply(ln, json_mode)]
            context = tool.lines[max(0, start - 8) :]
        assert not extra, f"second reply to {line!r}: {extra}; last lines: {context}"

    with open(tool.log_path.parent / log, "w") as out:
        prev_end, prev_line = None, None
        for kind, line in generate(rng):
            if prev_end is not None:
                no_extra_reply(prev_end, prev_line)
            out.write(line + "\n")
            sent.append(line)
            stats["lines"] += 1
            with tool._cond:
                start = len(tool.lines)
            tool.send(line)
            if kind == "proto":
                json_mode = line.endswith("json")
            prev_end = wait_reply(line, start)
            prev_line = line

    # Still alive and consistent
    if json_mode:
        tool.send("proto text")
        time.sleep(0.2)
    tool.sync()
    st = tool.cmd("status")
    assert st["mode"] in ("none", "unicast", "broadcast")
    cs = tool.cmd("cli stats")
    assert cs["lines_truncated"] == "0" and cs["fields_dropped"] == "0", cs.values
    print(f"fuzz: {stats}")
    assert stats["ok"] > LINES // 20, stats  # the generator also produces valid commands
