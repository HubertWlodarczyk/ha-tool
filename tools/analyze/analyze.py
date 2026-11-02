#!/usr/bin/env python3
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Static analysis of the project's own C sources (not Zephyr), from the compile commands of the
simulation builds:

    gcc -fanalyzer   path-sensitive checks (NULL dereference, leaks, use after free, ...)
    clang-tidy       bugprone / cert / clang-analyzer checks configured in .clang-tidy

Exits 1 if either reports anything. Build first (scripts/build.sh).
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILDS = ("ha_tool", "dut_sim")
OWN = ("ha_tool/src/", "dut_sim/src/", "common/src/")
# GCC options clang does not know (clang-tidy parses with clang)
GCC_ONLY = re.compile(
    r"^-(fno-reorder-functions|fno-defer-pop|fno-freestanding|fno-printf-return-value|"
    r"fno-strict-overflow|fno-asynchronous-unwind-tables|fmacro-prefix-map=.*|"
    r"fdiagnostics-color.*|specs=.*|mfp16-format=.*|Wno-volatile|fanalyzer|"
    r"fno-common|mabi=.*|mcpu=.*|mfpu=.*|mfloat-abi=.*|mtune=.*|"
    r"Wno-(pointer-sign|unused-but-set-variable)|Werror=implicit-int|fno-pie|"
    r"fno-pic|fno-builtin-.*|fsanitize=.*|fstack-usage|fdump-.*)$"
)


def own_commands() -> list[dict]:
    seen: dict[str, dict] = {}
    for app in BUILDS:
        db = ROOT / "build" / app / app / "compile_commands.json"
        if not db.exists():
            sys.exit(f"{db} missing: run scripts/build.sh")
        for entry in json.loads(db.read_text()):
            rel = os.path.relpath(entry["file"], ROOT)
            if rel.startswith(OWN) and rel not in seen:
                seen[rel] = entry
    return [seen[k] for k in sorted(seen)]


def args_of(entry: dict) -> list[str]:
    return entry.get("arguments") or shlex.split(entry["command"])


def run_gcc_analyzer(entry: dict) -> str:
    args = args_of(entry)
    out = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        out.append(a)
    cmd = [*out, "-fanalyzer", "-fdiagnostics-color=never", "-o", os.devnull]
    res = subprocess.run(cmd, cwd=entry["directory"], capture_output=True, text=True)
    return res.stderr


def run_clang_tidy(entry: dict) -> str:
    args = [a for a in args_of(entry)[1:] if not GCC_ONLY.match(a)]
    skip = False
    flags = []
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-c"):
            skip = a == "-o"
            continue
        if a == entry["file"]:
            continue
        flags.append(a)
    cmd = [
        "clang-tidy",
        "--quiet",
        f"--config-file={ROOT / '.clang-tidy'}",
        entry["file"],
        "--",
        "-m32",
        "-Wno-unknown-warning-option",
        "-Wno-unused-command-line-argument",
        *flags,
    ]
    res = subprocess.run(cmd, cwd=entry["directory"], capture_output=True, text=True)
    # only diagnostics located in our sources
    keep = []
    block = False
    for line in res.stdout.splitlines():
        m = re.match(r"^(/[^:]+):\d+:\d+: (warning|error):", line)
        if m:
            block = os.path.relpath(m.group(1), ROOT).startswith(OWN)
        if block:
            keep.append(line)
    return "\n".join(keep)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--only", choices=("gcc", "tidy"), help="run one tool")
    ap.add_argument("files", nargs="*", help="limit to these sources (relative paths)")
    opts = ap.parse_args()

    entries = own_commands()
    if opts.files:
        entries = [e for e in entries if os.path.relpath(e["file"], ROOT) in opts.files]

    tools = {"gcc": run_gcc_analyzer, "tidy": run_clang_tidy}
    if opts.only:
        tools = {opts.only: tools[opts.only]}

    failed = False
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        for name, fn in tools.items():
            results = pool.map(fn, entries)
            for entry, text in zip(entries, results):
                diag = [ln for ln in text.splitlines() if ln.strip()]
                if any(re.search(r"(warning|error):", ln) for ln in diag):
                    failed = True
                    print(f"== {name}: {os.path.relpath(entry['file'], ROOT)}")
                    print("\n".join(diag))
            print(f"{name}: {len(entries)} files checked", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
