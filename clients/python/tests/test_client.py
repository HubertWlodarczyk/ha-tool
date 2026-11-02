# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Unit tests of the hatool client on a scripted fake board (no hardware, no simulation)."""

from __future__ import annotations

import json
import queue
import threading
import time
from typing import Callable

import pytest

from hatool import HaTool, HaToolError
from hatool import _generated as spec
from hatool.core import build_line
from hatool.transport import Transport


class FakeBoard(Transport):
    """Answers command lines with JSON Lines produced by handler(line) -> list of str/dict."""

    def __init__(self, handler: Callable[[str], list] | None = None, chunk: int = 0):
        self.handler = handler or (lambda line: [])
        self.chunk = chunk  # split the output into reads of this size (0: whole lines)
        self.rx: queue.Queue[bytes] = queue.Queue()
        self.sent: list[str] = []
        self.closed = False

    # board side
    def emit(self, *items) -> None:
        data = b""
        for it in items:
            text = json.dumps(it) if isinstance(it, dict) else it
            data += (text + "\r\n").encode()
        if self.chunk:
            for i in range(0, len(data), self.chunk):
                self.rx.put(data[i : i + self.chunk])
        else:
            self.rx.put(data)

    def event(self, name: str, t: int = 1, **fields) -> None:
        self.emit({"type": "evt", "t": t, "evt": name, **{k: str(v) for k, v in fields.items()}})

    # client side
    def read(self, timeout: float) -> bytes:
        try:
            return self.rx.get(timeout=timeout)
        except queue.Empty:
            return b""

    def write(self, data: bytes) -> None:
        for line in data.decode().splitlines():
            self.sent.append(line)
            out = default_reply(line)
            if out is None:
                out = self.handler(line)
            self.emit(*out)

    def close(self) -> None:
        self.closed = True


def ok(**fields) -> dict:
    return {"type": "ok", **{k: str(v) for k, v in fields.items()}}


def default_reply(line: str) -> list | None:
    if line == "proto json":
        return [ok(proto="json")]
    if line == "version":
        return [ok(app="ha_tool", version="0.2.0", zephyr="4.5.0", proto=spec.PROTOCOL)]
    return None


def board(handler=None, **kw) -> tuple[HaTool, FakeBoard]:
    fake = FakeBoard(handler, **kw)
    return HaTool(fake, sync_timeout=2), fake


def test_sync_switches_to_json_and_checks_protocol():
    tool, fake = board()
    assert fake.sent[:2] == ["proto json", "version"]
    tool.close()
    assert fake.closed


def test_protocol_mismatch_is_refused():
    fake = FakeBoard()
    orig = fake.write

    def write(data: bytes) -> None:
        if data.decode().strip() == "version":
            fake.emit(ok(app="ha_tool", version="9", zephyr="x", proto=spec.PROTOCOL + 1))
            return
        orig(data)

    fake.write = write
    with pytest.raises(HaToolError, match="protocol mismatch"):
        HaTool(fake, sync_timeout=2)
    assert fake.closed


def test_reply_values_typed_and_data_rows():
    def handler(line):
        assert line == "bonding list"
        row = {"type": "data", "tag": "IDENTITY", "id": "1", "name": "A b", "bonds": "2"}
        return ["bonding list", row, ok(count=1)]  # echo noise first

    tool, _ = board(handler)
    rep = tool.bonding.list()
    assert rep.count == 1 and rep["count"] == "1"
    rows = rep.rows("IDENTITY")
    assert rows == [{"id": 1, "name": "A b", "bonds": 2}]
    tool.close()


def test_err_raises_with_errno_and_fields():
    tool, _ = board(lambda line: [{"type": "err", "errno": -22, "msg": "bad", "key": "x"}])
    with pytest.raises(HaToolError) as exc:
        tool.raw("broadcast 1 create bogus=1 bis=1")
    assert exc.value.errno == -22 and exc.value.msg == "bad" and exc.value.fields == {"key": "x"}
    err = tool.raw("broadcast 1 create bogus=1 bis=1", check=False)
    assert isinstance(err, HaToolError)
    tool.close()


@pytest.mark.parametrize(
    "text",
    [
        "nosuch: command not found",
        "list: wrong parameter count",
        "bcast: Please specify a subcommand.",
        "line ignored: 1 non-ASCII character(s)",
        'not terminated: "',
        "Too many arguments in the command.",
    ],
)
def test_shell_errors_end_the_command(text):
    tool, _ = board(lambda line: [text])
    with pytest.raises(HaToolError) as exc:
        tool.raw("whatever", timeout=2)
    assert exc.value.errno == -22
    tool.close()


def test_no_reply_times_out():
    tool, _ = board(lambda line: [])
    t0 = time.monotonic()
    with pytest.raises(TimeoutError):
        tool.raw("status", timeout=0.3)
    assert time.monotonic() - t0 < 2
    tool.close()


def test_events_during_a_command_and_wait_event():
    def handler(line):
        return [
            {"type": "evt", "t": 5, "evt": "broadcast_started", "big": "1", "encrypted": "0"},
            ok(queued=1),
        ]

    tool, fake = board(handler)
    tool.raw("broadcast 1 start")
    ev = tool.wait_event("broadcast_started", timeout=1, big=1, encrypted=False)
    assert ev.big == 1 and ev.encrypted is False and ev.t_ms == 5
    with pytest.raises(TimeoutError):
        tool.wait_event("broadcast_started", timeout=0.2, big=2)
    m = tool.mark()
    fake.event("broadcast_stopped", big=1, broadcast_id="0x123456", reason="0x16")
    ev = tool.wait_event(
        "broadcast_stopped", since=m, timeout=1, broadcast_id=lambda v: v == 0x123456
    )
    assert ev.reason == "0x16"
    tool.close()


def test_subscribe_and_listener_errors():
    tool, fake = board()
    seen = []
    tool.on_event(lambda e: (_ for _ in ()).throw(RuntimeError("listener bug")))
    tool.on_event(lambda e: seen.append(e.name))
    with tool.subscribe(timeout=1) as sub:
        fake.event("mode", **{"from": "none", "to": "unicast"})
        fake.event("mode", **{"from": "unicast", "to": "none"})
        got = [next(sub).values["to"], next(sub).values["to"]]
    assert got == ["unicast", "none"]
    time.sleep(0.1)
    assert seen == ["mode", "mode"]  # the failing listener did not stop the reader
    with tool.subscribe(timeout=0.2) as sub, pytest.raises(TimeoutError):
        next(sub)
    tool.close()


def test_lines_split_across_reads():
    def handler(line):
        return [{"type": "data", "tag": "BIG", "big": "1", "name": "Ż ó"}, ok(count=1)]

    tool, _ = board(handler, chunk=3)
    rep = tool.raw("broadcast list")
    assert rep.data[0]["name"] == "Ż ó"
    tool.close()


def test_commands_are_serialised_across_threads():
    lock = threading.Lock()

    def handler(line):
        with lock:
            return [ok(echo=line.split()[-1])]

    tool, _ = board(handler)
    results: dict[int, str] = {}

    def worker(i):
        results[i] = tool.raw(f"time {i}")["echo"]

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(20)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert results == {i: str(i) for i in range(20)}
    tool.close()


def test_command_line_quoting():
    line = build_line(
        "broadcast 2 create",
        [],
        {"name": 'A "b" \\c', "program": "zażółć", "code": None, "pbp": True},
    )
    assert line == (
        'broadcast 2 create name="A \\"b\\" \\\\c" '
        'program="za\\xc5\\xbc\\xc3\\xb3\\xc5\\x82\\xc4\\x87" pbp=1'
    )
    assert build_line("mode", ["unicast"], {}) == "mode unicast"
    assert build_line("x", [], {"k": ""}) == 'x k=""'


def test_generated_method_builds_the_line():
    tool, fake = board(
        lambda line: [
            ok(big=1, broadcast_id="0x1", name="T", preset="16_2", bis=1, encrypted=0, pbp="sq")
        ]
    )
    rep = tool.broadcast.create(2, name="T", bis=1)
    assert fake.sent[-1].startswith("broadcast 2 create") and "name=T bis=1" in fake.sent[-1]
    assert rep.big == 1 and rep.encrypted is False
    tool.broadcast.bis.tone(2, 3, freq=440)
    assert fake.sent[-1] == "broadcast 2 bis tone 3 freq=440"
    tool.broadcast.start_all()
    assert fake.sent[-1] == "broadcast start --all"
    tool.close()


def test_reply_buffer_does_not_grow():
    tool, _ = board(lambda line: ["echo " + line, ok()])
    for i in range(500):
        tool.raw(f"time {i}")
    assert len(tool._lines) < 5
    tool.close()


def test_flags_and_dashed_keys():
    """--flags are written when true; keys with a dash keep it on the wire."""
    assert build_line("status", [], {"--verbose": True}) == "status --verbose"
    assert build_line("status", [], {"--verbose": False}) == "status"
    line = build_line("bonding connect", ["Tel"], {"bluetooth-name": "Buds 2", "address": None})
    assert line == 'bonding connect Tel bluetooth-name="Buds 2"'

    seen = []
    tool, _ = board(lambda line: seen.append(line) or [ok(id=1, name="Tel")])
    tool.bonding.show("Tel", verbose=True)
    assert seen[-1] == "bonding show Tel --verbose"
    tool.close()
