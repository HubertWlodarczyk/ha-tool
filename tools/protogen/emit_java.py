# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Java client: command classes, option builders and protocol tables (io.hatool.gen)."""

from __future__ import annotations

import re

from spec import (
    COPYRIGHT,
    HEADER,
    SPDX,
    Node,
    camel,
    java_name,
    list_arg,
    notes,
    optional_args,
    reply_doc,
    required_args,
    required_flags,
    targets,
    tree,
    wire,
)
from writer import Code

PACKAGE = "io.hatool.gen"
WIDTH = 120
TYPES = {
    "int": "int",
    "float": "double",
    "bool": "boolean",
    "str": "String",
    "addr": "String",
    "hex": "String",
    "enum": "String",
}
IMPORTS = [
    "io.hatool.Reply",
    "java.util.ArrayList",
    "java.util.Collections",
    "java.util.HashMap",
    "java.util.LinkedHashMap",
    "java.util.List",
    "java.util.Map",
]


def _file(body: Code) -> str:
    """License, package, the imports the body uses, then the body."""
    text = body.text()
    head = Code(indent=4, width=WIDTH)
    head.raw(f"/*\n * {COPYRIGHT}\n *\n * {SPDX}\n */\n")
    head.line()
    head.line(f"// {HEADER}")
    head.line(f"package {PACKAGE};")
    head.line()
    used = {
        imp
        for imp in IMPORTS
        if re.search(r"(?<![A-Za-z])" + re.escape(imp.rsplit(".", 1)[1]) + r"\b", text)
    }
    for imp in sorted(set(used)):
        head.line(f"import {imp};")
    if used:
        head.line()
    return head.text() + text


def _enum_check(code: Code, name: str, var: str, values: list[str]) -> None:
    code.call("Check.oneOf", [f'"{name}"', var, *(f'"{v}"' for v in values)], ";")


def _options_class(c: dict, cls: str) -> str | None:
    opts, keys = optional_args(c), c.get("keys", [])
    flags = [f for f in c.get("flags", []) if not f.get("required")]
    if not opts and not keys and not flags:
        return None
    code = Code(indent=4, width=WIDTH)
    code.javadoc(f"Optional arguments of '{c['cmd']}': {c.get('doc', '')}")
    with code.block(f"public final class {cls} {{"):
        code.line("final List<Object> args = new ArrayList<>();")
        code.line("final Map<String, Object> keys = new LinkedHashMap<>();")
        for a in opts:
            code.line()
            code.javadoc(f"{a.get('doc', a['name'])} (positional)")
            with code.block(f"public {cls} {camel(a['name'])}({TYPES[a['type']]} v) {{"):
                code.line("args.add(v);")
                code.line("return this;")
        for k in keys:
            code.line()
            code.javadoc("; ".join([k["name"], *notes(k)]))
            setter = java_name(k["name"], c["cmd"])
            with code.block(f"public {cls} {setter}({TYPES[k['type']]} v) {{"):
                if k.get("values"):
                    _enum_check(code, k["name"], "v", k["values"])
                code.line(f'keys.put("{k["name"]}", v);')
                code.line("return this;")
        for f in flags:
            code.line()
            code.javadoc(f"--{f['name']}: {f.get('doc', '')}")
            with code.block(f"public {cls} {java_name(f['name'], c['cmd'])}() {{"):
                code.line(f'keys.put("--{f["name"]}", Boolean.TRUE);')
                code.line("return this;")
    return _file(code)


def _args_list(code: Code, req: list[dict], lst: dict | None) -> str:
    """Code collecting the positional arguments; returns the expression to pass."""
    if not req and lst is None:
        return "List.of()"
    if lst is None:
        return "List.of(" + ", ".join(camel(a["name"]) for a in req) + ")"
    code.line("List<Object> a = new ArrayList<>();")
    for a in req:
        code.line(f"a.add({camel(a['name'])});")
    code.line(f"a.addAll(List.of({camel(lst['name'])}));")
    return "a"


def _command(c: dict) -> str:
    """Java expression of the command line words, targets filled in: "broadcast " + big + " x"."""
    parts: list[str] = []
    lit: list[str] = []
    for w in wire(c).split():
        if w.startswith("{"):
            parts.append('"' + " ".join(lit) + ' "')
            parts.append(camel(w[1:-1]))
            lit = [""]
        else:
            lit.append(w)
    tail = " ".join(lit).rstrip()
    if tail:
        parts.append('"' + tail + '"')
    return " + ".join(p for p in parts if p != '""')


def _keys_expr(code: Code, c: dict, base: str) -> str:
    """Map of the key=value arguments, with the required --flags."""
    req = required_flags(c)
    if not req:
        return base
    if base == "Map.of()":
        return "Map.of(" + ", ".join(f'"--{f["name"]}", Boolean.TRUE' for f in req) + ")"
    code.line(f"Map<String, Object> k = new LinkedHashMap<>({base});")
    for f in req:
        code.line(f'k.put("--{f["name"]}", Boolean.TRUE);')
    return "k"


def _method(code: Code, word: str, c: dict, root: bool, files: dict[str, str]) -> None:
    target = "this" if root else "t"
    name = java_name(word, c["cmd"])
    req, lst = required_args(c), list_arg(c)
    params = [f"{TYPES[a['type']]} {camel(a['name'])}" for a in req]
    if lst is not None:
        params.append(f"String... {camel(lst['name'])}")
    enums = [a for a in req if a["type"] == "enum"]
    cls = camel(c["cmd"].replace(" ", "_"), upper=True) + "Options"
    options = _options_class(c, cls)
    doc = (c.get("doc", ""), reply_doc(c))
    plain = ", ".join(camel(a["name"]) for a in req)
    positional = [a for a in req if a["name"] not in targets(c)]
    command = _command(c)

    code.line()
    code.javadoc(*doc)
    if options is None:
        with code.method(f"public Reply {name}", params):
            for a in enums:
                _enum_check(code, a["name"], camel(a["name"]), a["values"])
            args = _args_list(code, positional, lst)
            keys = _keys_expr(code, c, "Map.of()")
            code.line(f"return {target}.call({command}, {args}, {keys});")
        return

    files[f"{cls}.java"] = options
    with code.method(f"public Reply {name}", [*params, f"{cls} o"]):
        for a in enums:
            _enum_check(code, a["name"], camel(a["name"]), a["values"])
        code.line(f"List<Object> a = new ArrayList<>({_args_list(Code(), positional, None)});")
        if lst is not None:
            code.line(f"a.addAll(List.of({camel(lst['name'])}));")
        code.line("a.addAll(o.args);")
        keys = _keys_expr(code, c, "o.keys")
        code.line(f"return {target}.call({command}, a, {keys});")
    if lst is not None:
        return

    code.line()
    code.javadoc(*doc)
    with code.method(f"public Reply {name}", params):
        code.line(f"return {name}({plain + ', ' if plain else ''}new {cls}());")

    opts = optional_args(c)
    if len(opts) == 1 and not c.get("keys") and not c.get("flags"):
        # Direct overload: mode("broadcast"), bonding().create("Phone")
        o = opts[0]
        positional_all = [a for a in c.get("args", []) if not a.get("list")]
        all_params = [f"{TYPES[a['type']]} {camel(a['name'])}" for a in positional_all]
        code.line()
        code.javadoc(*doc)
        with code.method(f"public Reply {name}", all_params):
            if o["type"] == "enum":
                _enum_check(code, o["name"], camel(o["name"]), o["values"])
            prefix = plain + ", " if plain else ""
            code.line(f"return {name}({prefix}new {cls}().{camel(o['name'])}({camel(o['name'])}));")


def _group(node: Node, cls: str, files: dict[str, str]) -> None:
    for group in node.groups.values():
        _group(group, group.class_stem + "Commands", files)
    root = node.path == ""
    code = Code(indent=4, width=WIDTH)
    if root:
        code.javadoc("Top level commands; io.hatool.HaTool extends this class.")
        head = f"public abstract class {cls} {{"
    else:
        code.javadoc(f"'{node.path}' commands")
        head = f"public final class {cls} {{"
    with code.block(head):
        if root:
            code.javadoc("Sends one command: path, positional arguments, key=value arguments.")
            code.line(
                "public abstract Reply call(String command, List<Object> args, "
                "Map<String, Object> keys);"
            )
        else:
            code.line("private final RootCommands t;")
        for word, group in node.groups.items():
            code.line(f"private final {group.class_stem}Commands {camel(word)};")
        code.line()
        if root:
            code.javadoc("Creates the command groups.")
            # The groups only keep the reference; nothing is sent before the subclass is built
            code.line('@SuppressWarnings("this-escape")')
            ctor = f"protected {cls}() {{"
        else:
            code.javadoc("Commands sent through t.")
            ctor = f"public {cls}(RootCommands t) {{"
        with code.block(ctor):
            if not root:
                code.line("this.t = t;")
            for word, group in node.groups.items():
                owner = "this" if root else "t"
                code.line(f"{camel(word)} = new {group.class_stem}Commands({owner});")
        for word, group in node.groups.items():
            code.line()
            code.javadoc(f"'{group.path}' commands")
            with code.block(f"public {group.class_stem}Commands {camel(word)}() {{"):
                code.line(f"return {camel(word)};")
        for word, c in node.commands.items():
            _method(code, word, c, root, files)
    files[f"{cls}.java"] = _file(code)


def _map_entries(code: Code, var: str, entries: dict[str, dict[str, str]]) -> None:
    """var.put("name", map("field", "type", ...)); one field per line when too long."""
    for name, fields in entries.items():
        pairs = [f'"{k}", "{v}"' for k, v in fields.items()]
        one = f'{var}.put("{name}", map({", ".join(pairs)}));'
        if len(code.prefix) + len(one) <= code.width:
            code.line(one)
            continue
        code.line(f'{var}.put("{name}", map(')
        with code.indented(), code.indented():
            for i, pair in enumerate(pairs):
                code.line(pair + ("," if i < len(pairs) - 1 else "));"))


def _protocol(spec: dict) -> str:
    code = Code(indent=4, width=WIDTH)
    code.javadoc("Protocol version, event names, field types (int, float, bool, str, addr, hex).")
    with code.block("public final class Protocol {"):
        code.line(f"public static final int VERSION = {spec['protocol']};")
        code.line()
        code.line("public static final Map<String, Map<String, String>> EVENT_FIELDS = events();")
        code.line("public static final Map<String, Map<String, String>> REPLY_FIELDS = replies();")
        code.javadoc("Command line of each command ({name}: a target filled in by the caller)")
        code.line("public static final Map<String, String> WIRES = wires();")
        code.line()
        code.javadoc("Event names")
        with code.block("public static final class Events {"):
            for name in spec["events"]:
                code.line(f'public static final String {name.upper()} = "{name}";')
            code.line()
            code.line("private Events() {")
            code.line("}")
        code.line()
        code.line("private Protocol() {")
        code.line("}")
        code.line()
        with code.block("private static Map<String, String> map(String... kv) {"):
            code.line("Map<String, String> m = new HashMap<>();")
            with code.block("for (int i = 0; i + 1 < kv.length; i += 2) {"):
                code.line("m.put(kv[i], kv[i + 1]);")
            code.line("return Collections.unmodifiableMap(m);")
        for fn, entries in (
            ("events", {n: e.get("fields", {}) for n, e in spec["events"].items()}),
            ("replies", {c["cmd"]: c.get("reply", {}) for c in spec["commands"]}),
        ):
            code.line()
            with code.block(f"private static Map<String, Map<String, String>> {fn}() {{"):
                code.line("Map<String, Map<String, String>> m = new HashMap<>();")
                _map_entries(code, "m", entries)
                code.line("return Collections.unmodifiableMap(m);")
        code.line()
        with code.block("private static Map<String, String> wires() {"):
            code.line("Map<String, String> m = new HashMap<>();")
            for c in spec["commands"]:
                code.line(f'm.put("{c["cmd"]}", "{wire(c)}");')
            code.line("return Collections.unmodifiableMap(m);")
    return _file(code)


def _check() -> str:
    code = Code(indent=4, width=WIDTH)
    code.javadoc("Client side validation of enum arguments.")
    with code.block("final class Check {"):
        code.line("private Check() {")
        code.line("}")
        code.line()
        with code.block("static void oneOf(String name, String v, String... values) {"):
            with code.block("for (String s : values) {"), code.block("if (s.equals(v)) {"):
                code.line("return;")
            code.line(
                'throw new IllegalArgumentException(name + "=" + v + ": one of " '
                '+ String.join(", ", values));'
            )
    return _file(code)


def generate(spec: dict) -> dict[str, str]:
    files: dict[str, str] = {}
    _group(tree(spec), "RootCommands", files)
    files["Protocol.java"] = _protocol(spec)
    files["Check.java"] = _check()
    return files
