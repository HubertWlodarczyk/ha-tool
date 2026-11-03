# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""docs/protocol.md: command and event reference."""

from __future__ import annotations

from spec import HEADER, notes, targets, wire


def _syntax(c: dict) -> str:
    words = [" ".join(f"<{w[1:-1]}>" if w.startswith("{") else w for w in wire(c).split())]
    for a in c.get("args", []):
        if a["name"] in targets(c):
            continue
        s = f"<{a['name']}{'...' if a.get('list') else ''}>"
        words.append(f"[{s}]" if a.get("optional") else s)
    words += [f"[{k['name']}=]" for k in c.get("keys", [])]
    words += [
        f"--{f['name']}" if f.get("required") else f"[--{f['name']}]" for f in c.get("flags", [])
    ]
    return " ".join(words)


def _command(c: dict) -> list[str]:
    out = [f"### `{_syntax(c)}`", "", c.get("doc", ""), ""]
    if c.get("mode"):
        out += [f"Mode: `{c['mode']}`", ""]
    rows = [(a["name"] + " (positional)", a) for a in c.get("args", [])]
    rows += [(k["name"] + "=", k) for k in c.get("keys", [])]
    if rows:
        out += ["| Argument | Type | Notes |", "|---|---|---|"]
        for name, a in rows:
            n = notes(a) + (["optional"] if a.get("optional") else [])
            text = "; ".join(n).replace("|", "\\|")
            out.append(f"| `{name}` | {a['type']} | {text} |")
        out.append("")
    if c.get("reply"):
        opt = set(c.get("reply_optional", []))
        out += [
            "Reply: "
            + ", ".join(
                f"`{k}` ({v}{', optional' if k in opt else ''})" for k, v in c["reply"].items()
            ),
            "",
        ]
    for tag, fields in c.get("data", {}).items():
        out += [f"Data `{tag}`: " + ", ".join(f"`{k}` ({v})" for k, v in fields.items()), ""]
    return out


def generate(spec: dict) -> str:
    types = ", ".join(f"`{k}` {v}" for k, v in spec["types"].items())
    out = [
        f"<!-- {HEADER} -->",
        "",
        f"# leabench CLI protocol (version {spec['protocol']})",
        "",
        spec.get("doc", ""),
        "",
        f"Types: {types}.",
        "",
        "## Commands",
        "",
    ]
    for c in spec["commands"]:
        out += _command(c)
    out += ["## Events", "", "| Event | Fields | Notes |", "|---|---|---|"]
    for name, e in spec["events"].items():
        opt = set(e.get("optional", []))
        fields = ", ".join(
            f"`{k}` ({v}{', optional' if k in opt else ''})" for k, v in e.get("fields", {}).items()
        )
        out.append(f"| `{name}` | {fields} | {e.get('doc', '')} |")
    return "\n".join(out) + "\n"
