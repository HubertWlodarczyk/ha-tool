# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Client core: transport, reader thread, JSON Lines protocol, replies and events.

The board is switched to 'proto json' when the client connects: every reply and event is one JSON
object per line. Commands are plain text lines (values with spaces are quoted).
"""

from __future__ import annotations

import json
import queue
import threading
import time
from collections.abc import Iterator
from dataclasses import dataclass, field
from typing import Any, Callable

from . import _generated as spec
from .transport import Transport

# Errors the Zephyr shell prints itself, before a command handler runs (plain text)
_SHELL_ERRORS = (
    "command not found",
    "wrong parameter count",
    "specify a subcommand",
    "non-ASCII character",
    "not terminated",
    "Too many arguments",
)
EINVAL = -22


class HaToolError(Exception):
    """A command answered ERR (or the shell rejected it)."""

    def __init__(self, command: str, errno: int, msg: str, fields: dict[str, str]):
        super().__init__(f"'{command}' -> ERR {errno} {msg} {fields if fields else ''}".rstrip())
        self.command = command
        self.errno = errno
        self.msg = msg
        self.fields = fields


def _convert(value: str | None, kind: str) -> Any:
    if value is None:
        return None
    try:
        if kind == "int":
            return int(value, 0)
        if kind == "float":
            return float(value)
        if kind == "bool":
            return value not in ("0", "", "no", "off")
        if kind == "hex":
            return int(value, 16)
    except ValueError:
        pass
    return value


class _Typed:
    """Values as strings (obj["k"]) or converted by the protocol spec (obj.k)."""

    _types: dict[str, str]
    values: dict[str, str]

    def __getitem__(self, key: str) -> str:
        return self.values[key]

    def get(self, key: str, default: Any = None) -> Any:
        return self.values.get(key, default)

    def __getattr__(self, key: str) -> Any:
        values = self.__dict__.get("values", {})
        if key not in values:
            raise AttributeError(key)
        return _convert(values[key], self.__dict__.get("_types", {}).get(key, "str"))

    def __dir__(self) -> list[str]:
        return [*super().__dir__(), *self.__dict__.get("values", {})]

    def int(self, key: str) -> int:
        return int(self.values[key], 0)


@dataclass
class Reply(_Typed):
    """Final OK of a command, with the data lines (DEV, BIS, ...) printed before it."""

    command: str
    values: dict[str, str]
    data: list[dict[str, str]] = field(default_factory=list)
    _types: dict[str, str] = field(default_factory=dict, repr=False)

    def rows(self, tag: str) -> list[dict[str, Any]]:
        """Data lines of one tag, values converted by the spec."""
        types = spec.DATA.get(self.command, {}).get(tag, {})
        return [
            {k: _convert(v, types.get(k, "str")) for k, v in d.items() if k != "tag"}
            for d in self.data
            if d.get("tag") == tag
        ]


@dataclass
class Event(_Typed):
    name: str
    t_ms: int
    values: dict[str, str]
    _types: dict[str, str] = field(default_factory=dict, repr=False)


def _quote(value: Any) -> str:
    """A command argument, quoted if needed. Non-ASCII characters go as UTF-8 \\xHH escapes:
    the Zephyr shell only takes ASCII input and decodes the escapes inside quotes."""
    if isinstance(value, bool):
        return "1" if value else "0"
    s = str(value)
    if s and s.isascii() and not any(c in s for c in ' "\\'):
        return s
    out = []
    for ch in s:
        if ch in '"\\':
            out.append("\\" + ch)
        elif ch.isascii():
            out.append(ch)
        else:
            out.extend(f"\\x{b:02x}" for b in ch.encode())
    return '"' + "".join(out) + '"'


def build_line(command: str, args: list[Any], keys: dict[str, Any]) -> str:
    """'bonding pair' + positional args + key=value (None values are left out); a key
    "--name" is a flag, written as --name when true."""
    parts = [command] + [_quote(a) for a in args if a is not None]
    for k, v in keys.items():
        if k.startswith("--"):
            if v:
                parts.append(k)
        elif v is not None:
            parts.append(f"{k}={_quote(v)}")
    return " ".join(parts)


# --- Client ---


class Client:
    """Connection to one ha_tool board. Thread safe: commands are sent one at a time."""

    def __init__(self, transport: Transport, log: Callable[[str], None] | None = None):
        self._t = transport
        self._log = log
        self._cond = threading.Condition()
        self._lines: list[Any] = []  # parsed JSON objects (or plain text strings)
        self.events: list[Event] = []  # every event received, in order
        self._listeners: list[Callable[[Event], None]] = []
        self._queues: list[queue.Queue] = []
        self._cmd_lock = threading.Lock()
        self._mark = 0
        self._stop = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True, name="hatool-rx")
        self._reader.start()

    # --- reader ---

    def _read_loop(self) -> None:
        buf = b""
        while not self._stop:
            try:
                chunk = self._t.read(0.1)
            except (OSError, ConnectionError, ValueError):
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip("\r\x00 ")
                if line:
                    self._on_line(line)

    def _on_line(self, line: str) -> None:
        if self._log:
            self._log("<< " + line)
        obj: Any = line
        if line.startswith("{"):
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                obj = line
        ev = None
        if isinstance(obj, dict) and obj.get("type") == "evt":
            values = {k: str(v) for k, v in obj.items() if k not in ("type", "t", "evt")}
            name = str(obj.get("evt"))
            ev = Event(name, int(obj.get("t", 0)), values, spec.EVENTS.get(name, {}))
        with self._cond:
            if ev is not None:
                self.events.append(ev)
            else:
                self._lines.append(obj)
            self._cond.notify_all()
        if ev is not None:
            for q in list(self._queues):
                q.put(ev)
            for cb in list(self._listeners):
                try:
                    cb(ev)
                except Exception:  # a listener must not stop the reader
                    pass

    # --- lifecycle ---

    def close(self) -> None:
        self._stop = True
        self._reader.join(timeout=1)
        self._t.close()

    def __enter__(self) -> Client:
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def sync(self, timeout: float = 20.0) -> Reply:
        """Waits until the board answers, switches it to JSON Lines and checks the protocol."""
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.raw("proto json", timeout=1.0)
                break
            except (TimeoutError, HaToolError) as err:
                if time.monotonic() > deadline:
                    raise TimeoutError("board not responding") from err
                self._send("")
        ver = self.raw("version")
        if ver.int("proto") != spec.PROTOCOL:
            raise HaToolError(
                "version",
                EINVAL,
                "protocol mismatch",
                {"board": ver["proto"], "client": str(spec.PROTOCOL)},
            )
        return ver

    # --- commands ---

    def _send(self, line: str) -> None:
        if self._log:
            self._log(">> " + line)
        self._t.write((line + "\n").encode())

    def raw(self, line: str, timeout: float = 10.0, check: bool = True) -> Reply:
        """Sends a command line and waits for its final reply. ERR raises HaToolError (check)."""
        command = _command_of(line)
        with self._cmd_lock:
            with self._cond:
                start = len(self._lines)
                self._mark = len(self.events)
            self._send(line)
            deadline = time.monotonic() + timeout
            data: list[dict[str, str]] = []
            idx = start
            with self._cond:
                while True:
                    while idx < len(self._lines):
                        obj = self._lines[idx]
                        idx += 1
                        if isinstance(obj, str):
                            if any(e in obj for e in _SHELL_ERRORS):
                                self._trim(idx)
                                return self._fail(line, EINVAL, obj, {}, check)
                            continue  # echo / text noise
                        kind = obj.get("type")
                        fields = {
                            k: str(v) for k, v in obj.items() if k not in ("type", "errno", "msg")
                        }
                        if kind == "data":
                            data.append(fields)
                        elif kind == "ok":
                            self._trim(idx)
                            return Reply(command, fields, data, spec.REPLIES.get(command, {}))
                        elif kind == "err":
                            self._trim(idx)
                            return self._fail(
                                line,
                                int(obj.get("errno", EINVAL)),
                                str(obj.get("msg", "")),
                                fields,
                                check,
                            )
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError(f"no reply to '{line}'")
                    self._cond.wait(remaining)

    def _trim(self, consumed: int) -> None:
        """Drops the reply lines up to a finished command (_cond held): only lines of the
        next command are needed, so long sessions do not grow the buffer."""
        del self._lines[:consumed]

    @staticmethod
    def _fail(line: str, errno: int, msg: str, fields: dict[str, str], check: bool):
        err = HaToolError(line, errno, msg, fields)
        if check:
            raise err
        return err

    def call(
        self,
        command: str,
        args: list[Any] | None = None,
        keys: dict[str, Any] | None = None,
        timeout: float = 10.0,
    ) -> Reply:
        return self.raw(build_line(command, args or [], keys or {}), timeout=timeout)

    # --- events ---

    def mark(self) -> int:
        """Position in the event list, for wait_event(since=...)."""
        with self._cond:
            return len(self.events)

    def wait_event(
        self, name: str, timeout: float = 30.0, since: int | None = None, **match: Any
    ) -> Event:
        """Waits for an event (default: received after the last command was sent).

        match: event values that must be equal (compared as strings), or predicates
        taking the converted value.
        """
        idx = self._mark if since is None else since
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                while idx < len(self.events):
                    ev = self.events[idx]
                    idx += 1
                    if ev.name == name and all(_matches(ev, k, m) for k, m in match.items()):
                        return ev
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"no event {name} {match} within {timeout} s")
                self._cond.wait(remaining)

    def events_since(self, since: int = 0, name: str | None = None) -> list[Event]:
        with self._cond:
            return [e for e in self.events[since:] if name is None or e.name == name]

    def subscribe(self, timeout: float | None = None) -> Subscription:
        """Iterator over the events received from this call on (blocks for the next one;
        TimeoutError after `timeout` s without an event). close() or `with` ends it."""
        return Subscription(self, timeout)

    def on_event(self, callback: Callable[[Event], None]) -> None:
        """callback(event) from the reader thread for every event (keep it short)."""
        self._listeners.append(callback)


class Subscription:
    """Events from the moment of subscribing, in order (see Client.subscribe)."""

    def __init__(self, client: Client, timeout: float | None):
        self._client = client
        self._timeout = timeout
        self._q: queue.Queue = queue.Queue()
        client._queues.append(self._q)

    def __iter__(self) -> Iterator[Event]:
        return self

    def __next__(self) -> Event:
        try:
            return self._q.get(timeout=self._timeout)
        except queue.Empty:
            raise TimeoutError(f"no event within {self._timeout} s") from None

    def close(self) -> None:
        if self._q in self._client._queues:
            self._client._queues.remove(self._q)

    def __enter__(self) -> Subscription:
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def _matches(ev: Event, key: str, m: Any) -> bool:
    if callable(m):
        return m(getattr(ev, key, None) if key in ev.values else None)
    return ev.values.get(key) == (("1" if m else "0") if isinstance(m, bool) else str(m))


def _command_of(line: str) -> str:
    """Command of the specification a line runs ('broadcast 2 bis add ...' -> 'broadcast bis
    add'): the longest command line pattern that matches, a {target} word matching any value."""
    words = line.split()
    best, best_len, best_targets = (words[0] if words else ""), 0, 0
    for cmd, wire in spec.WIRES.items():
        pattern = wire.split()
        if len(pattern) > len(words):
            continue
        ok = all(
            (w.startswith("{") and "=" not in t and not t.startswith("--")) or w == t
            for w, t in zip(pattern, words)
        )
        n_targets = sum(w.startswith("{") for w in pattern)
        if ok and (
            len(pattern) > best_len or (len(pattern) == best_len and n_targets < best_targets)
        ):
            best, best_len, best_targets = cmd, len(pattern), n_targets
    return best
