# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Python client for ha_tool, the nRF5340 DK tool for hearing aid connectivity tests.

    from hatool import HaTool

    # or HaTool.find(), HaTool.tcp("localhost", 7000)
    with HaTool.open("/dev/ttyACM0") as tool:
        d = tool.bonding.create("Phone")
        tool.bonding.connect(d.id, address="AA:BB:CC:DD:EE:FF/random")
        tool.wait_event("bonding_connected", id=d.id, timeout=20)

Every command of the CLI is a method (tool.bcast.bis.add(...), tool.le_source.call.incoming(...)),
generated from protocol/hatool.yaml; see docs/protocol.md. Errors raise HaToolError.
"""

from __future__ import annotations

import os
from typing import Callable

from ._generated import PROTOCOL, Commands
from .core import Client, Event, HaToolError, Reply, build_line
from .transport import FdTransport, SerialTransport, TcpTransport, Transport

__all__ = [
    "PROTOCOL",
    "Event",
    "HaTool",
    "HaToolError",
    "Reply",
    "Transport",
    "build_line",
]
__version__ = "0.2.0"

SEGGER_VID = 0x1366  # J-Link OB of the nRF5340 DK


class HaTool(Client, Commands):
    """One ha_tool board. Commands are methods; events: wait_event, subscribe, on_event."""

    def __init__(
        self,
        transport,
        log: Callable[[str], None] | None = None,
        sync: bool = True,
        sync_timeout: float = 20.0,
    ):
        Client.__init__(self, transport, log)
        Commands.__init__(self, self)
        if sync:
            try:
                self.sync(sync_timeout)
            except Exception:
                self.close()
                raise

    @classmethod
    def open(cls, port: str, baud: int = 1_000_000, **kw) -> HaTool:
        """Serial port of a DK (/dev/ttyACM0, COM3) or the pty of a simulated board."""
        if port.startswith("/dev/pts/") or baud == 0:
            return cls(FdTransport(port, None), **kw)
        try:
            return cls(SerialTransport(port, baud), **kw)
        except ImportError:
            if os.name != "posix":
                raise
            return cls(FdTransport(port, baud), **kw)

    @classmethod
    def tcp(cls, host: str = "localhost", port: int = 7000, **kw) -> HaTool:
        """A simulated board through scripts/sim.py --tcp."""
        return cls(TcpTransport(host, port), **kw)

    @classmethod
    def find(cls, chip_id: str | None = None, baud: int = 1_000_000, **kw) -> HaTool:
        """First DK (SEGGER USB VCOM) answering as ha_tool, optionally with a given 'id'."""
        from serial.tools import list_ports  # pyserial

        errors = []
        for p in sorted(list_ports.comports(), key=lambda p: p.device):
            if p.vid != SEGGER_VID:
                continue
            try:
                tool = cls.open(p.device, baud, sync_timeout=2.0, **kw)
            except Exception as e:  # other VCOM of the J-Link, or another firmware
                errors.append(f"{p.device}: {e}")
                continue
            if chip_id is None or tool.id().id == chip_id:
                return tool
            tool.close()
        raise RuntimeError("no ha_tool board found" + (": " + "; ".join(errors) if errors else ""))
