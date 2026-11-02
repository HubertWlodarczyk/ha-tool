# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Firmware command table: ha_tool/src/cli_table.c (table + shell root words) and
ha_tool/src/cli_cmds.h (handler prototypes). The dispatcher is common/src/cli_tree.c."""

from __future__ import annotations

from spec import COPYRIGHT, HEADER, SPDX, list_arg, optional_args, required_args

GROUP_HELP = {
    "bonding": "Virtual identities, connections and bonds (unicast)",
    "le-source": "LE Audio source on the attached identity (unicast)",
    "broadcast": "Auracast broadcasts: BIGs, subgroups, BISes",
    "dev": "Virtual devices",
    "phone": "Phone simulation on the selected virtual device",
    "bcast": "Auracast broadcast sources",
    "evt": "Event filter",
    "cli": "CLI introspection",
}


def handler(c: dict) -> str:
    return "cmd_" + "_".join(c["cmd"].replace("-", "_").split())


def path(c: dict) -> str:
    """Words of the command line; a target argument appears as {name} (wire: in the spec)."""
    return c.get("wire", c["cmd"])


def usage(c: dict) -> str:
    targets = {w[1:-1] for w in path(c).split() if w.startswith("{")}
    words = []
    for a in c.get("args", []):
        if a["name"] in targets:
            continue
        s = f"<{a['name']}{'...' if a.get('list') else ''}>"
        words.append(f"[{s}]" if a.get("optional") else s)
    words += [f"[{k['name']}=]" for k in c.get("keys", [])]
    words += [f"[--{f['name']}]" for f in c.get("flags", [])]
    return " ".join(words)


def c_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def ident(word: str) -> str:
    return word.replace("-", "_")


def header(spec: dict) -> str:
    out = [
        "/*",
        f" * {COPYRIGHT}",
        " *",
        f" * {SPDX}",
        " */",
        "",
        f"/* {HEADER} */",
        "",
        "#ifndef CLI_CMDS_H_",
        "#define CLI_CMDS_H_",
        "",
        "#include <stddef.h>",
        "",
        "#include <zephyr/shell/shell.h>",
        "",
    ]
    for c in spec["commands"]:
        out.append(f"int {handler(c)}(const struct shell *sh, size_t argc, char **argv);")
    out += ["", "#endif /* CLI_CMDS_H_ */", ""]
    return "\n".join(out)


def table(spec: dict) -> str:
    out = [
        "/*",
        f" * {COPYRIGHT}",
        " *",
        f" * {SPDX}",
        " */",
        "",
        f"/* {HEADER} */",
        "",
        "#include <zephyr/shell/shell.h>",
        "",
        '#include "cli_cmds.h"',
        '#include "cli_tree.h"',
        "",
    ]
    entries = []
    for i, c in enumerate(spec["commands"]):
        keys = [k["name"] for k in c.get("keys", [])]
        flags = [f["name"] for f in c.get("flags", [])]
        if keys:
            out.append(
                f"static const char *const keys_{i}[] = {{"
                + ", ".join(c_str(k) for k in keys)
                + ", NULL};"
            )
        if flags:
            out.append(
                f"static const char *const flags_{i}[] = {{"
                + ", ".join(c_str(f) for f in flags)
                + ", NULL};"
            )
        lo = len(required_args(c))
        hi = "CLI_ARGS_LIST" if list_arg(c) else str(lo + len(optional_args(c)))
        entries.append(
            [
                f"\t{{.path = {c_str(path(c))},",
                f"\t .handler = {handler(c)},",
                f"\t .args_min = {lo},",
                f"\t .args_max = {hi},",
                f"\t .keys = {'keys_' + str(i) if keys else 'NULL'},",
                f"\t .flags = {'flags_' + str(i) if flags else 'NULL'},",
                f"\t .usage = {c_str(usage(c))},",
                f"\t .help = {c_str(c.get('doc', ''))}}},",
            ]
        )
    out += ["", "const struct cli_cmd cli_cmds[] = {"]
    for e in entries:
        out += e
    out += ["};", "", "const size_t cli_cmds_count = ARRAY_SIZE(cli_cmds);", ""]

    # one shell root word per first word of the commands
    roots: dict[str, str] = {}
    for c in spec["commands"]:
        word = path(c).split()[0]
        if word not in roots:
            single = len(path(c).split()) == 1
            roots[word] = GROUP_HELP.get(word, c.get("doc", "") if single else "")
    out.append("/* Shell root words: every line goes to cli_dispatch() */")
    for word, help_text in sorted(roots.items()):
        name = ident(word)
        out += [
            f"static const struct shell_static_entry cli_root_{name} = {{",
            f"\t.syntax = {c_str(word)},",
            f"\t.help = {c_str(help_text or word)},",
            "\t.subcmd = NULL,",
            "\t.handler = cli_dispatch,",
            "\t.args = {.mandatory = 1, .optional = SHELL_OPT_ARG_CHECK_SKIP},",
            "};",
            f"static const TYPE_SECTION_ITERABLE(union shell_cmd_entry, shell_cmd_{name}, "
            f"shell_root_cmds, shell_cmd_{name}) = {{.entry = &cli_root_{name}}};",
            "",
        ]
    return "\n".join(out).rstrip("\n") + "\n"


def generate(spec: dict) -> dict[str, str]:
    return {"cli_cmds.h": header(spec), "cli_table.c": table(spec)}
