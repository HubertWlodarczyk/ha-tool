# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Text CLI client of one board (simulated pty or a DK serial port), used by the tests.

CLI protocol (common/include/cli.h, protocol/leabench.yaml):
    OK [k=v ...]            final reply, success
    ERR <errno> <message>   final reply, failure
    EVT t=<ms> <name> k=v   asynchronous event
    <TAG> k=v ...           data lines printed before OK (DEV, BOND, ...)
"""

from __future__ import annotations

import os
import pathlib
import re
import select
import shlex
import termios
import threading
import time
import tty
from dataclasses import dataclass, field

from . import conformance

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Errors reported by the Zephyr shell itself (before our handlers run)
SHELL_ERR_RE = re.compile(
    r"command not found|wrong parameter count|specify a subcommand|non-ASCII character|"
    r"not terminated|Too many arguments"
)


def split_line(line: str) -> list[str]:
    """Words of a k=v line; values may be "quoted" (names with spaces)."""
    try:
        return shlex.split(line)
    except ValueError:
        return line.split()


def parse_kv(tokens: list[str]) -> dict[str, str]:
    out = {}
    for tok in tokens:
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


class CliError(Exception):
    pass


@dataclass
class Reply:
    ok: bool
    values: dict[str, str]
    data: list[dict[str, str]] = field(default_factory=list)
    raw: str = ""
    errno: int = 0

    def __getitem__(self, key: str) -> str:
        return self.values[key]

    def int(self, key: str) -> int:
        return int(self.values[key], 0)


@dataclass
class Event:
    name: str
    t_ms: int
    values: dict[str, str]
    raw: str

    def __getitem__(self, key: str) -> str:
        return self.values[key]

    def int(self, key: str) -> int:
        return int(self.values[key], 0)


class Device:
    """One board's CLI: a simulated board (pty) or a real DK (serial port, baud given)."""

    def __init__(
        self,
        name: str,
        port: str,
        log_path: pathlib.Path,
        baud: int | None = None,
        app: str | None = None,
    ):
        self.name = name
        # leabench replies and events are checked against the protocol spec (conformance.py)
        self.conform = app == "leabench"
        self.log_path = log_path
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        if baud is not None:
            attrs = termios.tcgetattr(self.fd)
            speed = getattr(termios, f"B{baud}")
            attrs[4] = attrs[5] = speed
            attrs[2] &= ~termios.CRTSCTS
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.lines: list[str] = []
        self.events: list[Event] = []
        self._cond = threading.Condition()
        self._evt_mark = 0
        # Open for the life of the device, closed in close()
        self._cli_log = open(log_path.with_suffix(".cli.log"), "w", buffering=1)  # noqa: SIM115
        self._stop = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self) -> None:
        buf = b""
        while not self._stop:
            try:
                r, _, _ = select.select([self.fd], [], [], 0.1)
                if not r:
                    continue
                chunk = os.read(self.fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = ANSI.sub("", raw.decode(errors="replace")).strip("\r\x00 ")
                if not line:
                    continue
                self._cli_log.write(f"<< {line}\n")
                with self._cond:
                    self.lines.append(line)
                    if line.startswith("EVT "):
                        tokens = split_line(line)
                        values = parse_kv(tokens[3:])
                        t_ms = int(tokens[1].split("=", 1)[1])
                        self.events.append(Event(tokens[2], t_ms, values, line))
                        if self.conform:
                            conformance.record_event(tokens[2], values, line)
                    self._cond.notify_all()

    def close(self) -> None:
        self._stop = True
        self._reader.join(timeout=1)
        try:
            os.close(self.fd)
        except OSError:
            pass
        self._cli_log.close()

    def sync(self, timeout: float = 20) -> None:
        """Waits until the CLI answers (the boot banner may have been missed)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                self.cmd("version", timeout=1)
                return
            except (TimeoutError, CliError):
                self.send("")
        raise TimeoutError(f"{self.name}: CLI not responding")

    # --- commands ---

    def send(self, line: str) -> None:
        self._cli_log.write(f">> {line}\n")
        os.write(self.fd, (line + "\n").encode())

    def cmd(self, line: str, timeout: float = 10.0, check: bool = True) -> Reply:
        """Sends a command and waits for its final OK/ERR line."""
        with self._cond:
            start = len(self.lines)
            self._evt_mark = len(self.events)
        self.send(line)
        deadline = time.monotonic() + timeout
        data: list[dict[str, str]] = []
        idx = start
        with self._cond:
            while True:
                while idx < len(self.lines):
                    ln = self.lines[idx]
                    idx += 1
                    tokens = ln.split() if ln.startswith("ERR") else split_line(ln)
                    if tokens[0] == "OK":
                        rep = Reply(True, parse_kv(tokens[1:]), data, ln)
                        if self.conform:
                            conformance.record_reply(line, rep.values, data, ln)
                        return rep
                    if tokens[0] != "ERR" and SHELL_ERR_RE.search(ln):
                        tokens = ["ERR", "-22", *tokens]
                    if tokens[0] == "ERR":
                        errno = int(tokens[1]) if len(tokens) > 1 else 0
                        # k=v fields of the message, e.g. hint="..."
                        values = parse_kv(split_line(ln)[2:]) if ln.startswith("ERR") else {}
                        rep = Reply(False, values, data, ln, errno)
                        if check:
                            raise CliError(f"{self.name}: '{line}' -> {ln}")
                        return rep
                    if tokens[0] != "EVT" and tokens[0].isupper():
                        d = parse_kv(tokens[1:])
                        d["_tag"] = tokens[0]
                        data.append(d)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"{self.name}: no reply to '{line}'")
                self._cond.wait(remaining)

    # --- events ---

    def mark(self) -> int:
        with self._cond:
            return len(self.events)

    def wait_evt(self, evt: str, timeout: float = 30.0, since: int | None = None, **match) -> Event:
        """Waits for an event (after `since`, default: the last command sent).

        Keyword arguments must match event values; a callable is used as a predicate.
        """
        idx = self._evt_mark if since is None else since
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                while idx < len(self.events):
                    ev = self.events[idx]
                    idx += 1
                    if ev.name == evt and all(
                        (m(ev.values.get(k)) if callable(m) else ev.values.get(k) == str(m))
                        for k, m in match.items()
                    ):
                        return ev
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    recent = "\n  ".join(e.raw for e in self.events[-15:])
                    raise TimeoutError(
                        f"{self.name}: no event {evt} {match} within {timeout}s. "
                        f"Recent:\n  {recent}"
                    )
                self._cond.wait(remaining)

    def events_named(self, name: str, since: int = 0) -> list[Event]:
        with self._cond:
            return [e for e in self.events[since:] if e.name == name]
