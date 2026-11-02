# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""How many BIGs / BISes ha_tool runs at once, plain and encrypted, and the air time budget that
predicts it (broadcast budget, refusal at broadcast <n> start). The simulated hearing aid checks
every BIS of every BIG for loss and the right tone."""

import pytest

from helpers import SYNC_TIMEOUT, big_tones, check_big, create_big, near

# The largest configurations the budget accepts (16_2, rtn=0; BIG 1 plain, the others encrypted)
EDGE = [(8, 3), (7, 4), (6, 5), (3, 3, 2), (4, 3, 1), (2, 3, 3)]


def test_most_bises(pair):
    """11 BISes at once: a plain BIG of 8 and an encrypted BIG of 3, all without loss. The budget
    predicts it and refuses one more BIG."""
    ha, dut = pair
    assert ha.cmd("broadcast budget preset=16_2 rtn=0")["next_big_bis"] == "8"
    create_big(ha, 1, 8)
    create_big(ha, 2, 3, code="Secret2")
    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", big=1, timeout=15)
    assert ha.cmd("broadcast budget preset=16_2 rtn=0 enc=1")["next_big_bis"] == "3"
    ha.cmd("broadcast 2 start")
    ha.wait_evt("broadcast_started", big=2, timeout=15)

    budget = ha.cmd("broadcast budget preset=8_2 rtn=0")
    assert budget["next_big_bis"] == "0" and int(budget["free_us"]) < 2500, budget
    create_big(ha, 3, 1, preset="8_2")
    rep = ha.cmd("broadcast 3 start", check=False)
    assert rep.errno == -7 and "budget_us=" in rep.raw  # -E2BIG with the numbers

    check_big(dut, "Cap1", big_tones(1, 8))
    check_big(dut, "Cap2", big_tones(2, 3), code="Secret2")


def test_three_bigs_plain_and_encrypted(pair):
    """Three BIGs at once, plain and encrypted with different codes; a code of another BIG is
    rejected (MIC failure), so the codes do not mix up."""
    ha, dut = pair
    codes = {1: None, 2: "Secret2", 3: "Other3"}
    for b, code in codes.items():
        create_big(ha, b, 1, code=code)
    for b in codes:
        ha.cmd(f"broadcast {b} start")
        ha.wait_evt("broadcast_started", big=b, timeout=15)
    assert ha.cmd("broadcast budget preset=16_2 rtn=0")["next_big_bis"] == "0"

    for b, code in codes.items():
        check_big(dut, f"Cap{b}", big_tones(b, 1), code=code)

    m = dut.mark()
    dut.cmd("bcast scan name=Cap3 code=Secret2")
    assert dut.wait_evt("dut_bcast_stopped", since=m, timeout=SYNC_TIMEOUT)["cause"] == "wrong_code"


def test_encryption_costs_air_time(pair):
    """The 4 octet MIC of encrypted PDUs: a BIG that just fits in plain does not fit encrypted."""
    ha, _ = pair
    ha.cmd("broadcast 1 create name=Edge preset=16_2 bis=6 rtn=2")
    assert ha.cmd("broadcast 1 show")["fits"] == "1"
    plain = int(ha.cmd("broadcast 1 show")["airtime_us"])
    ha.cmd("broadcast 1 delete")
    ha.cmd("broadcast 1 create name=Edge preset=16_2 bis=6 rtn=2 code=Secret")
    show = ha.cmd("broadcast 1 show")
    assert int(show["airtime_us"]) == plain + 6 * 3 * 4 * 4  # 4 octets, 2M, rtn=2, 6 BISes
    assert show["fits"] == "0"
    assert ha.cmd("broadcast 1 start", check=False).errno == -7


def start_bigs(ha, cfg: tuple[int, ...]) -> None:
    for b, n in enumerate(cfg, start=1):
        create_big(ha, b, n, code=None if b == 1 else f"Edge{b}")
    for b in range(1, len(cfg) + 1):
        ha.cmd(f"broadcast {b} start")
        ha.wait_evt("broadcast_started", big=b, timeout=15)


def test_budget_edge(tool):
    """The budget stops one BIS past the edge, with two BIGs (12 BISes) and three (9)."""
    start_bigs(tool, (8, 3))
    create_big(tool, 3, 1, code="Edge3")
    assert tool.cmd("broadcast 3 start", check=False).errno == -7
    tool.cmd("broadcast 2 stop")
    tool.wait_evt("broadcast_stopped", big=2, timeout=15)
    tool.cmd("broadcast 2 delete")
    create_big(tool, 2, 4, code="Edge2")
    assert tool.cmd("broadcast 2 start", check=False).errno == -7  # 8 + 4

    tool.cmd("reset")
    start_bigs(tool, (3, 3, 2))
    tool.cmd("broadcast 3 stop")
    tool.wait_evt("broadcast_stopped", big=3, timeout=15)
    tool.cmd("broadcast 3 delete")
    create_big(tool, 3, 3, code="Edge3")
    assert tool.cmd("broadcast 3 start", check=False).errno == -7  # 3 + 3 + 3


@pytest.mark.slow
@pytest.mark.parametrize("cfg", EDGE, ids=lambda c: "+".join(map(str, c)))
def test_budget_edge_lossless(pair, cfg):
    """Every configuration at the edge of the budget: each BIG is found and received without loss
    for 4 s by a receiver that stays synchronized to the periodic advertising (the hard case:
    the train must not sit next to the BIG either)."""
    ha, dut = pair
    start_bigs(ha, cfg)
    for b, n in enumerate(cfg, start=1):
        m = dut.mark()
        dut.cmd(f"bcast scan name=Cap{b}" + ("" if b == 1 else f" code=Edge{b}"))
        dut.wait_evt("dut_audio", since=m, src=f"bcast_bis{n}", timeout=SYNC_TIMEOUT)
        for _ in range(4):
            m = dut.mark()
            for i, f in enumerate(big_tones(b, n), start=1):
                ev = dut.wait_evt("dut_audio", since=m, src=f"bcast_bis{i}", timeout=10)
                assert ev["lost"] == "0" and near(f)(ev["freq"]), (cfg, b, i, ev.raw)
        dut.cmd("bcast stop")
