# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Helpers shared by the tests: tone matching, virtual device connections, broadcasts."""

from __future__ import annotations

import pathlib
import time

from simlib import Device

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "tests" / "out"  # logs of every test
SYNC_TIMEOUT = 30  # s: a broadcast sink finds, syncs to and starts a BIG
STREAM_TIMEOUT = 45  # s: discovery + unicast stream setup, simulated at real-time pace


def near(hz: int, tol: int | None = None):
    """Matcher: measured tone frequency within tol Hz (default 3 %, at least 15 Hz)."""
    tol = tol if tol is not None else max(15, hz * 3 // 100)
    return lambda v: v is not None and abs(int(v) - hz) <= tol


# --- Virtual devices ---


def connect(ha: Device, dut_addr: str, name: str | None = None, pair: bool = True) -> int:
    """Creates a virtual device, connects it to the DUT and (optionally) bonds."""
    rep = ha.cmd("bonding create" + (f" {name}" if name else ""))
    dev_id = rep.int("id")
    ha.cmd(f"bonding connect {dev_id} address={dut_addr}")
    ha.wait_evt("bonding_connected", id=dev_id, timeout=20)
    if pair:
        m = ha.mark()
        ha.cmd(f"bonding pair {dev_id}")
        ha.wait_evt("bonding_security", since=m, id=dev_id, timeout=20)
        # Key distribution (LTK, IRK, identity address) continues after encryption: wait for
        # it, a disconnection now would abort the bonding
        ha.wait_evt("bonding_paired", since=m, id=dev_id, bonded=1, timeout=20)
    return dev_id


def disconnect(ha: Device, dev_id: int) -> None:
    ha.cmd(f"bonding disconnect {dev_id}")
    ha.wait_evt("bonding_disconnected", id=dev_id, timeout=20)


# --- Broadcasts ---


def start_broadcast(ha: Device, args: str, tones: tuple[int, ...] = (500, 1000)) -> None:
    """'broadcast 1 create <args>' (one BIS unless preset= or bis= given), one tone per BIS,
    start."""
    if "bis=" not in args and "preset=" not in args:
        args += " bis=1"
    ha.cmd(f"broadcast 1 create {args}")
    for i, f in enumerate(tones, start=1):
        ha.cmd(f"broadcast 1 bis tone {i} freq={f}")
    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", timeout=15)


def big_tones(big: int, n: int) -> list[int]:
    """Distinct tones for the BISes of every BIG: 300, 400, ... Hz."""
    return [300 + 100 * (8 * (big - 1) + i) for i in range(n)]


def create_big(ha: Device, big: int, n: int, code: str | None = None, preset: str = "16_2") -> None:
    """BIG 'Cap<big>' of n BISes (rtn=0), tones from big_tones()."""
    ha.cmd(
        f"broadcast {big} create name=Cap{big} preset={preset} bis={n} rtn=0"
        + (f" code={code}" if code else "")
    )
    for i, f in enumerate(big_tones(big, n), start=1):
        ha.cmd(f"broadcast {big} bis tone {i} freq={f}")


def check_big(dut: Device, name: str, tones: list[int], code: str | None = None) -> None:
    """The DUT syncs to one BIG; every BIS has the right tone and no loss over a second."""
    m = dut.mark()
    dut.cmd(f"bcast scan name={name}" + (f" code={code}" if code else ""))
    dut.wait_evt("dut_audio", since=m, src=f"bcast_bis{len(tones)}", timeout=SYNC_TIMEOUT)
    m = dut.mark()
    for i, freq in enumerate(tones, start=1):
        ev = dut.wait_evt("dut_audio", since=m, src=f"bcast_bis{i}", timeout=10)
        assert near(freq)(ev["freq"]) and ev["lost"] == "0", (name, i, ev)
    dut.cmd("bcast stop")


# --- Raw protocol ---


def raw_lines(dev: Device, line: str, settle: float = 0.5) -> list[str]:
    """Sends a line without waiting for OK (other protocols), returns what came back."""
    start = len(dev.lines)
    dev.send(line)
    time.sleep(settle)
    return dev.lines[start:]
