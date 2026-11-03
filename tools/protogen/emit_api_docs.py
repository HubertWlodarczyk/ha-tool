# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Generate language-specific command references."""

from __future__ import annotations

from spec import HEADER, camel, java_name, notes, py_name


def _reply(c: dict) -> str:
    fields = ", ".join(f"`{k}` ({v})" for k, v in c.get("reply", {}).items())
    return f"Returns `Reply`: {fields or 'no fields'}."


def _arguments(c: dict) -> list[str]:
    rows = []
    for a in [*c.get("args", []), *c.get("keys", []), *c.get("flags", [])]:
        kind = "argument" if a in c.get("args", []) else "option"
        detail = "; ".join(notes(a))
        rows.append(f"| `{a['name']}` | `{a.get('type', 'bool')}` | {kind}; {detail} |")
    return rows


def _python_call(c: dict) -> str:
    words = c["cmd"].split()
    parent = "tool" + "".join(f".{py_name(w)}" for w in words[:-1])
    args = [py_name(a["name"]) for a in c.get("args", [])]
    keys = [f"{py_name(k['name'])}=..." for k in c.get("keys", [])]
    keys += [f"{py_name(f['name'])}=True" for f in c.get("flags", [])]
    values = ", ".join(args + keys)
    return f"{parent}.{py_name(words[-1])}({values})"


def _java_call(c: dict) -> str:
    words = c["cmd"].split()
    parent = "tool" + "".join(f".{camel(w)}()" for w in words[:-1])
    args = [camel(a["name"]) for a in c.get("args", [])]
    options = [*c.get("keys", []), *c.get("flags", [])]
    if options:
        cls = camel(c["cmd"].replace(" ", "_"), upper=True) + "Options"
        opts = f"new {cls}()" + "".join(
            f".{camel(o['name'])}()"
            if o in c.get("flags", [])
            else f".{camel(o['name'])}(...)" for o in options
        )
        args.append(opts)
    return f"{parent}.{java_name(words[-1], c['cmd'])}({', '.join(args)})"


def generate(spec: dict, language: str) -> str:
    title = "Python" if language == "python" else "Java"
    intro = (
        "Each command is a generated method. Arguments and accepted values come from the protocol "
        "specification. "
    )
    if language == "python":
        intro += "Replies are `Reply` objects; use attributes or `rows(tag)` for data rows."
    else:
        intro += "Replies are `Reply` objects; use `getInt`, `getString`, `getDouble`, or `getBool`."
    out = [f"<!-- {HEADER} -->", "", f"# leabench {title} command reference", "", intro, ""]
    for c in spec["commands"]:
        out += [f"## `{c['cmd']}`", "", c.get("doc", ""), "", "Example:", "", "```"]
        out += [_python_call(c) if language == "python" else _java_call(c), "```", ""]
        rows = _arguments(c)
        if rows:
            out += ["| Name | Type | Meaning and accepted values |", "|---|---|---|", *rows, ""]
        out += [_reply(c), ""]
        for tag, fields in c.get("data", {}).items():
            fields_text = ", ".join(f"`{k}` ({v})" for k, v in fields.items())
            out += [f"Data `{tag}`: {fields_text}.", ""]
    out += ["## Events", "", "| Event | Fields |", "|---|---|"]
    for name, event in spec["events"].items():
        fields = ", ".join(f"`{k}` ({v})" for k, v in event.get("fields", {}).items())
        out.append(f"| `{name}` | {fields} |")
    return "\n".join(out) + "\n"
