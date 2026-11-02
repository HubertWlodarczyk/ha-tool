#!/usr/bin/env python3
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Generates the protocol artefacts from protocol/hatool.yaml (the single source):

    clients/python/hatool/_generated.py         types + command methods of the Python client
    clients/java/src/main/java/io/hatool/gen/   command classes of the Java client
    docs/protocol.md                            command and event reference
    ha_tool/src/cli_table.c, cli_cmds.h         firmware command table (clang-formatted)

    tools/protogen/gen.py           regenerate
    tools/protogen/gen.py --check   fail if a generated file is out of date (CI, scripts/lint.sh)

Needs PyYAML (developer machine only; the generated files are committed).
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

import emit_c
import emit_java
import emit_markdown
import emit_python
import spec as protocol

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = ROOT / "protocol/hatool.yaml"
PY_OUT = ROOT / "clients/python/hatool/_generated.py"
JAVA_DIR = ROOT / "clients/java/src/main/java/io/hatool/gen"
DOC_OUT = ROOT / "docs/protocol.md"
C_DIR = ROOT / "ha_tool/src"


def clang_format(path: pathlib.Path, text: str) -> str:
    return subprocess.run(
        ["clang-format", f"--assume-filename={path}"],
        input=text,
        capture_output=True,
        text=True,
        check=True,
        cwd=ROOT,
    ).stdout


def outputs(spec: dict) -> dict[pathlib.Path, str]:
    out = {PY_OUT: emit_python.generate(spec), DOC_OUT: emit_markdown.generate(spec)}
    out.update({JAVA_DIR / name: text for name, text in emit_java.generate(spec).items()})
    out.update(
        {
            C_DIR / name: clang_format(C_DIR / name, text)
            for name, text in emit_c.generate(spec).items()
        }
    )
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--check", action="store_true", help="only check that outputs are current")
    args = ap.parse_args()

    files = outputs(protocol.load(SPEC))
    stale = [p for p, text in files.items() if not p.exists() or p.read_text() != text]
    obsolete = [p for p in JAVA_DIR.glob("*.java") if p not in files]
    if args.check:
        for p in stale + obsolete:
            print(f"out of date: {p.relative_to(ROOT)}")
        return 1 if stale or obsolete else 0

    for p, text in files.items():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    for p in obsolete:
        p.unlink()
    print(f"{len(files)} files written, {len(stale)} changed, {len(obsolete)} removed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
