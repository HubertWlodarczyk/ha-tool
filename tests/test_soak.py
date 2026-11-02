# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Long runs (pytest -m slow): broadcast soak with live changes, start/stop and connection
cycles (nothing may leak), event bursts (losses reported exactly).

Environment: HA_SOAK_S (soak length in simulated seconds, default 300), HA_CYCLES (default
200 broadcast / half as many connection cycles).
"""

from __future__ import annotations

import os
import random
import time

import pytest

from helpers import SYNC_TIMEOUT, big_tones, check_big, connect, create_big, near

pytestmark = pytest.mark.slow

SOAK_S = int(os.environ.get("HA_SOAK_S", "300"))
CYCLES = int(os.environ.get("HA_CYCLES", "200"))


def sent(ha, big: int) -> list[int]:
    return [int(d["sent"]) for d in ha.cmd(f"broadcast {big} show").data if d["_tag"] == "BIS"]


def test_broadcast_soak(pair):
    """Two BIGs (6 plain + 3 encrypted BISes) for SOAK_S seconds; tone and level of one BIS
    change every second. The hearing aid follows the plain BIG: no frame lost, ever; every
    BIS keeps sending one SDU per interval."""
    ha, dut = pair
    create_big(ha, 1, 6)
    create_big(ha, 2, 3, code="Soak2")
    for b in (1, 2):
        ha.cmd(f"broadcast {b} start")
        ha.wait_evt("broadcast_started", big=b, timeout=15)

    tones = big_tones(1, 6)
    m = dut.mark()
    dut.cmd("bcast scan name=Cap1")
    dut.wait_evt("dut_audio", since=m, src="bcast_bis6", timeout=SYNC_TIMEOUT)
    first = {1: sent(ha, 1), 2: sent(ha, 2)}
    t0 = dut.events[-1].t_ms
    mark = dut.mark()

    rng = random.Random(7)
    start = time.monotonic()
    while time.monotonic() - start < SOAK_S:
        bis = rng.randint(1, 6)
        tones[bis - 1] = rng.choice(range(200, 2000, 50))
        level = rng.randint(-30, 0)
        ha.cmd(f"broadcast 1 bis tone {bis} freq={tones[bis - 1]} level={level}")
        time.sleep(1)

    # every report since the start: complete and lossless
    reports = [e for e in dut.events[mark:] if e.name == "dut_audio"]
    assert len(reports) >= 6 * (SOAK_S - 2), len(reports)
    bad = [e.raw for e in reports if e["lost"] != "0" or e["frames"] != "100"]
    assert not bad, bad[:5]

    # the last tone of every BIS is heard
    m = dut.mark()
    for i, f in enumerate(tones, start=1):
        ev = dut.wait_evt("dut_audio", since=m, src=f"bcast_bis{i}", timeout=5)
        assert near(f)(ev["freq"]), (i, f, ev.raw)

    # one SDU per 10 ms on every BIS of both BIGs (simulated time from the reports)
    elapsed_ms = dut.events[-1].t_ms - t0
    for b in (1, 2):
        for before, after in zip(first[b], sent(ha, b)):
            rate = (after - before) * 1000 / elapsed_ms
            assert 95 <= rate <= 105, (b, rate)
    assert ha.cmd("cli stats")["evt_dropped"] == "0"


def test_broadcast_start_stop_cycles(pair):
    """CYCLES starts and stops of a 2-BIS BIG, then the broadcast still works: no ISO channel,
    advertising set or buffer leaks."""
    ha, dut = pair
    create_big(ha, 1, 2)
    for i in range(CYCLES):
        m = ha.mark()
        ha.cmd("broadcast 1 start")
        ha.wait_evt("broadcast_started", since=m, big=1, timeout=15)
        ha.cmd("broadcast 1 stop")
        ha.wait_evt("broadcast_stopped", since=m, big=1, timeout=15)
        assert ha.cmd("broadcast 1 show")["state"] != "streaming", i

    # the full capacity is still there
    ha.cmd("broadcast 1 delete")
    create_big(ha, 1, 6)
    create_big(ha, 2, 3, code="Cyc2")
    for b in (1, 2):
        ha.cmd(f"broadcast {b} start")
        ha.wait_evt("broadcast_started", big=b, timeout=15)
    check_big(dut, "Cap1", big_tones(1, 6))
    check_big(dut, "Cap2", big_tones(2, 3), code="Cyc2")


def test_connection_cycles(setup):
    """CYCLES / 2 rounds of create, connect, pair, disconnect, delete; the hearing aid keeps
    only its newest bonds. Afterwards a new device still connects and pairs."""
    ha, _, addr = setup
    for i in range(CYCLES // 2):
        dev = connect(ha, addr, name=f"c{i}")
        ha.cmd(f"bonding disconnect {dev}")
        ha.wait_evt("bonding_disconnected", id=dev, timeout=20)
        ha.cmd(f"bonding delete {dev}")
        assert ha.cmd("bonding list")["count"] == "0", i

    dev = connect(ha, addr, name="last")
    assert next(d for d in ha.cmd("bonding list").data if d["id"] == str(dev))["bonds"] == "1"


def test_event_burst(tool):
    """Bursts larger than the event queue: every event is either printed or counted in a
    'dropped' event, and 'cli stats' totals the losses."""
    lost_total = 0
    for n in (16, 32, 64, 200, 1000):
        m = tool.mark()
        tool.cmd(f"cli burst n={n}")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            got = len(tool.events_named("burst", since=m))
            lost = sum(int(e["n"]) for e in tool.events_named("dropped", since=m))
            if got + lost == n:
                break
            time.sleep(0.05)
        idx = [int(e["i"]) for e in tool.events_named("burst", since=m)]
        assert idx == sorted(idx) and len(set(idx)) == len(idx), "order / duplicates"
        assert got + lost == n, (n, got, lost)
        lost_total += lost
    assert int(tool.cmd("cli stats")["evt_dropped"]) == lost_total
    # still answers and reports events normally afterwards
    m = tool.mark()
    tool.cmd("cli burst n=4")
    time.sleep(0.5)
    assert len(tool.events_named("burst", since=m)) == 4
