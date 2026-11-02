# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Python client: protocol tables and one method per command (clients/python/hatool)."""

from __future__ import annotations

import json
import pprint
import textwrap

from spec import (
    COPYRIGHT,
    HEADER,
    SPDX,
    Node,
    list_arg,
    notes,
    optional_flags,
    py_name,
    reply_doc,
    required_flags,
    targets,
    tree,
    wire,
)
from writer import Code

TYPES = {
    "int": "int",
    "float": "float",
    "bool": "bool",
    "str": "str",
    "addr": "str",
    "hex": "int | str",
    "enum": "str",
}

PRELUDE = '''\
# {copyright}
# {spdx}

"""{header}"""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .core import Reply

PROTOCOL = {protocol}

'''

ENUM_CHECK = """\
def _enum(name, value, values):
    if value is not None and str(value) not in values:
        raise ValueError(f"{name}={value!r}: one of {', '.join(values)}")
"""


def _table(name: str, kind: str, value: dict) -> str:
    body = pprint.pformat(value, width=96, sort_dicts=False)
    return f"{name}: {kind} = {body}\n"


def q(value: str | list[str]) -> str:
    """Python literal with double quotes: "x" or ("a", "b")."""
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    return "(" + ", ".join(q(v) for v in value) + ("," if len(value) == 1 else "") + ")"


def _signature(c: dict) -> tuple[list[str], list[str]]:
    """Parameters and call arguments. Optional positionals before a required one (bcast
    txpower [<b>] <dBm>) are keyword-only; key=value arguments are keyword-only."""
    args = c.get("args", [])
    params, call_args, kwonly = ["self"], [], []
    for i, a in enumerate(args):
        n, t = py_name(a["name"]), TYPES[a["type"]]
        if a["name"] in targets(c):  # in the command path, not an argument after it
            params.append(f"{n}: {t}")
            continue
        if a.get("list"):
            params.append(f"*{n}: {t}")
            call_args.append(f"*{n}")
        elif a.get("optional"):
            later_required = any(not x.get("optional") and not x.get("list") for x in args[i + 1 :])
            (kwonly if later_required else params).append(f"{n}: {t} | None = None")
            call_args.append(n)
        else:
            params.append(f"{n}: {t}")
            call_args.append(n)
    keys = [f"{py_name(k['name'])}: {TYPES[k['type']]} | None = None" for k in c.get("keys", [])]
    keys += [f"{py_name(f['name'])}: bool = False" for f in optional_flags(c)]
    if list_arg(c) is None:
        params.append("*")
    params += kwonly + keys + ["reply_timeout: float = 10.0"]
    return params, call_args


def _docstring(code: Code, c: dict) -> None:
    lines = [c.get("doc", "")]
    for a in c.get("args", []):
        if a.get("optional") and a.get("doc"):
            lines.append(f"{a['name']}: {a['doc']}")
    for k in c.get("keys", []):
        n = notes(k)
        lines.append(f"{k['name']}: {', '.join(n)}" if n else "")
    for f in optional_flags(c):
        lines.append(f"{py_name(f['name'])}: --{f['name']}, {f.get('doc', '')}")
    lines.append(reply_doc(c))
    text = [part for line in lines if line for part in code.wrapped(line)]
    if len(text) == 1:
        code.line(f'"""{text[0]}"""')
        return
    code.line(f'"""{text[0]}')
    for t in text[1:]:
        code.line(t)
    code.line('"""')


def _method(code: Code, name: str, c: dict) -> None:
    params, call_args = _signature(c)
    code.call(f"def {py_name(name)}", params, " -> Reply:", trailing_comma=True)
    with code.indented():
        _docstring(code, c)
        for a in [*c.get("args", []), *c.get("keys", [])]:
            if a["type"] == "enum" and not a.get("list"):
                code.line(f"_enum({q(a['name'])}, {py_name(a['name'])}, {q(a['values'])})")
        pairs = [f"{q(k['name'])}: {py_name(k['name'])}" for k in c.get("keys", [])]
        pairs += [f"{q('--' + f['name'])}: {py_name(f['name'])}" for f in optional_flags(c)]
        pairs += [f"{q('--' + f['name'])}: True" for f in required_flags(c)]
        keys = "{" + ", ".join(pairs) + "}"
        # the command line words, targets filled in: f"broadcast {big} start"
        line = wire(c)
        for name in targets(c):
            line = line.replace("{" + name + "}", "{" + py_name(name) + "}")
        command = ("f" + q(line)) if targets(c) else q(line)
        code.call(
            "return self._c.call",
            [command, f"[{', '.join(call_args)}]", keys, "timeout=reply_timeout"],
            "",
            trailing_comma=True,
        )


def _class(code: Code, node: Node, name: str) -> None:
    for group in node.groups.values():
        _class(code, group, f"_{group.class_stem}")
    code.line(f"class {name}:")
    with code.indented():
        code.line(f'"""{node.path or "Top level"} commands"""')
        code.line()
        code.line("def __init__(self, client):")
        with code.indented():
            code.line("self._c = client")
            for word, group in node.groups.items():
                code.line(f"self.{py_name(word)} = _{group.class_stem}(client)")
        for word, c in node.commands.items():
            code.line()
            _method(code, word, c)
    code.line()
    code.line()


def generate(spec: dict) -> str:
    code = Code(indent=4, width=100)
    code.raw(
        PRELUDE.format(copyright=COPYRIGHT, spdx=SPDX, header=HEADER, protocol=spec["protocol"])
    )
    code.line()
    replies = {c["cmd"]: c.get("reply", {}) for c in spec["commands"]}
    data = {c["cmd"]: c["data"] for c in spec["commands"] if c.get("data")}
    events = {n: e.get("fields", {}) for n, e in spec["events"].items()}
    code.raw("# Field types of replies, data lines and events (int, float, bool, str, addr, hex)")
    code.raw(_table("REPLIES", "dict[str, dict[str, str]]", replies))
    code.raw(_table("DATA", "dict[str, dict[str, dict[str, str]]]", data))
    code.raw(_table("EVENTS", "dict[str, dict[str, str]]", events))
    code.raw("# Command line of each command ({name}: a target filled in by the caller)")
    code.raw(_table("WIRES", "dict[str, str]", {c["cmd"]: wire(c) for c in spec["commands"]}))
    code.line()
    code.raw(textwrap.dedent(ENUM_CHECK))
    code.line()
    code.line()
    _class(code, tree(spec), "Commands")
    return code.text()
