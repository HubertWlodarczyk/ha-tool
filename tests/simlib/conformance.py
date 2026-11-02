# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Protocol conformance over the whole test session: every OK reply (with its data lines) and
every event of ha_tool seen by any test is checked against protocol/hatool.yaml.

Errors: names or fields missing from the spec, spec fields missing from a reply or event,
values that do not parse as their spec type. Also reported (not an error): commands and
events of the spec that no test produced.
"""

from __future__ import annotations

import pathlib
import re
from collections import defaultdict

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = yaml.safe_load((ROOT / "protocol/hatool.yaml").read_text())
COMMANDS = {c["cmd"]: c for c in SPEC["commands"]}
EVENTS: dict[str, dict] = SPEC.get("events", {})
SAMPLES = 40  # kept per command / event (enough to see every variant, bounded memory)

TYPE_RE = {
    "int": re.compile(r"-?\d+"),
    "bool": re.compile(r"[01]"),
    "hex": re.compile(r"0x[0-9A-Fa-f]+"),
    "addr": re.compile(r"([0-9A-F]{2}:){5}[0-9A-F]{2}/(random|public)|none"),
    "float": re.compile(r"-?\d+(\.\d+)?"),
}

replies: dict[str, list[tuple[dict, list, str]]] = defaultdict(list)
events: dict[str, list[tuple[dict, str]]] = defaultdict(list)
seen_commands: set[str] = set()


def command_of(line: str) -> str | None:
    """Spec command a line runs: the longest command line pattern ('broadcast {big} start')."""
    words = line.split()
    best, best_len, best_targets = None, 0, 0
    for name, c in COMMANDS.items():
        pattern = c.get("wire", name).split()
        if len(pattern) > len(words):
            continue
        ok = all(
            (w.startswith("{") and "=" not in t and not t.startswith("--")) or w == t
            for w, t in zip(pattern, words)
        )
        n_targets = sum(w.startswith("{") for w in pattern)
        if ok and (
            len(pattern) > best_len or (len(pattern) == best_len and n_targets < best_targets)
        ):
            best, best_len, best_targets = name, len(pattern), n_targets
    return best


def record_reply(line: str, values: dict, data: list[dict], raw: str) -> None:
    cmd = command_of(line)
    if cmd is None:
        return
    seen_commands.add(cmd)
    if len(replies[cmd]) < SAMPLES:
        replies[cmd].append((dict(values), [dict(d) for d in data], raw))


def record_event(name: str, values: dict, raw: str) -> None:
    if len(events[name]) < SAMPLES or name not in EVENTS:
        events[name].append((dict(values), raw))


def _check_fields(
    where: str, spec: dict, values: dict, raw: str, errors: list[str], optional=()
) -> None:
    for k, v in values.items():
        if k not in spec:
            errors.append(f"{where}: field '{k}' not in the spec: {raw}")
            continue
        rx = TYPE_RE.get(spec[k])
        if rx is not None and v != "none" and not rx.fullmatch(v):
            errors.append(f"{where}: {k}={v!r} is not {spec[k]}: {raw}")
    for k in spec:
        if k not in values and k not in optional:
            errors.append(f"{where}: spec field '{k}' missing: {raw}")


def report() -> tuple[list[str], list[str]]:
    errors: list[str] = []
    for cmd, samples in sorted(replies.items()):
        spec = COMMANDS[cmd]
        for values, data, raw in samples:
            _check_fields(
                f"reply of '{cmd}'",
                spec.get("reply") or {},
                values,
                raw,
                errors,
                spec.get("reply_optional", ()),
            )
            for d in data:
                tag = d.get("_tag")
                tags = spec.get("data") or {}
                if tag not in tags:
                    errors.append(f"data of '{cmd}': tag {tag} not in the spec: {d}")
                    continue
                fields = {k: v for k, v in d.items() if k != "_tag"}
                _check_fields(f"{tag} line of '{cmd}'", tags[tag], fields, str(d), errors)
    for name, samples in sorted(events.items()):
        if name not in EVENTS:
            errors.append(f"event '{name}' not in the spec: {samples[0][1]}")
            continue
        for values, raw in samples:
            ev = EVENTS[name]
            _check_fields(
                f"event '{name}'",
                ev.get("fields") or {},
                values,
                raw,
                errors,
                ev.get("optional", ()),
            )

    info = []
    unused_cmds = sorted(set(COMMANDS) - seen_commands)
    unused_evts = sorted(set(EVENTS) - set(events))
    if unused_cmds:
        info.append(f"commands no test ran: {', '.join(unused_cmds)}")
    if unused_evts:
        info.append(f"events no test produced: {', '.join(unused_evts)}")
    # Same message from several samples once
    return list(dict.fromkeys(errors)), info
