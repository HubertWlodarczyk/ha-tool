# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Output limits: long real lines fit, too long lines are cut at a word boundary and marked
(text: truncated=1, JSON: "truncated":true, always valid JSON), and 'cli stats' counts them."""

import json

from helpers import raw_lines


def stats(tool) -> dict[str, int]:
    return {k: int(v) for k, v in tool.cmd("cli stats").values.items()}


def test_longest_real_lines_fit(tool):
    tool.cmd(
        'broadcast 1 create name="Hall \\"A\\" \\\\ meeting rooms 12345" preset=48_6 bis=8 rtn=0'
    )
    tool.cmd('broadcast 1 subgroup set 1 program="Program \\"X\\" 123"')
    assert tool.cmd("broadcast 1 show").data  # 8 BIS lines
    raw_lines(tool, "proto json")
    try:
        lines = []
        for c in ("broadcast 1 show", "cli spec", "limits", "broadcast list", "status"):
            lines += raw_lines(tool, c, settle=0.8)
        objs = [json.loads(ln) for ln in lines if ln.startswith("{")]
        assert len(objs) > 80 and all("truncated" not in o for o in objs)
        show = [o for o in objs if o.get("tag") == "BIS"]
        assert len(show) == 8
    finally:
        tool.cmd("proto text")
    assert stats(tool) == {"lines_truncated": 0, "fields_dropped": 0, "evt_dropped": 0}


def test_too_long_line_is_marked(tool):
    key = "k" * 220
    rep = tool.cmd(f"broadcast 1 create {key}=1 bis=1", check=False)
    assert rep.errno == -22 and rep.raw.endswith(" truncated=1")
    assert "allowed=" in rep.raw or "cmd=" in rep.raw  # cut at a word boundary
    assert stats(tool)["lines_truncated"] == 1

    raw_lines(tool, "proto json")
    try:
        (line,) = [
            ln for ln in raw_lines(tool, f"broadcast 1 create {key}=1 bis=1") if ln.startswith("{")
        ]
    finally:
        tool.cmd("proto text")
    obj = json.loads(line)  # still valid JSON
    assert obj["type"] == "err" and obj["truncated"] is True and obj["key"] == key
    assert stats(tool)["lines_truncated"] == 2
