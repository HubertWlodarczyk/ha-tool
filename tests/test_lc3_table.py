# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Precomputed LC3 frame table (ha_tool/src/lc3_frames.c): every BAP configuration (7.5 / 10 ms,
8..48 kHz incl. 44.1), tone selection on the table's grid, and the level set by rewriting the
global gain field. The simulated hearing aid decodes and measures each stream."""

import pytest

from helpers import SYNC_TIMEOUT, near

CONFIGS = [
    "8_1",
    "8_2",
    "16_1",
    "16_2",
    "24_1",
    "24_2",
    "32_1",
    "32_2",
    "441_1",
    "441_2",
    "48_1",
    "48_2",
    "48_3",
    "48_4",
    "48_5",
    "48_6",
]
RATE = {"8": 8000, "16": 16000, "24": 24000, "32": 32000, "441": 44100, "48": 48000}


@pytest.mark.parametrize("cfg", CONFIGS)
def test_every_configuration(pair, cfg):
    """Low and high tone (and a live change to a middle one) decoded right in every config."""
    ha, dut = pair
    rate = RATE[cfg.split("_")[0]]
    high = 3500 if rate == 8000 else 7000

    ha.cmd(f"broadcast 1 create name=Cfg{cfg} preset={cfg} bis=2 rtn=1")
    tones = {}
    for bis, freq in ((1, 200), (2, high)):
        ok = ha.cmd(f"broadcast 1 bis tone {bis} freq={freq}")
        tones[bis] = float(ok["freq_actual"])
        assert abs(tones[bis] - freq) <= 35, ok  # grid: 46..67 Hz
    show = ha.cmd("broadcast 1 show")
    assert [b["cfg"] for b in show.data if b["_tag"] == "BIS"] == [cfg, cfg]

    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", timeout=15)

    m = dut.mark()
    dut.cmd(f"bcast scan name=Cfg{cfg}")
    dut.wait_evt(
        "dut_bcast_stream",
        since=m,
        state="started",
        bis=2,
        sample_rate=str(rate),
        timeout=SYNC_TIMEOUT,
    )
    for bis in (1, 2):
        ev = dut.wait_evt(
            "dut_audio", since=m, src=f"bcast_bis{bis}", freq=near(round(tones[bis])), timeout=10
        )
        assert int(ev["lost"]) == 0 and int(ev["active"]) == 100, ev

    m = dut.mark()
    mid = float(ha.cmd("broadcast 1 bis tone 1 freq=1000")["freq_actual"])
    dut.wait_evt("dut_audio", since=m, src="bcast_bis1", freq=near(round(mid)), timeout=10)


def test_tone_selection(pair):
    """Nearest tone of the grid, exact=1, range limits and configs outside of the table."""
    ha, _ = pair
    ha.cmd("broadcast 1 create name=Sel")
    ha.cmd("broadcast 1 subgroup add preset=16_2")

    ok = ha.cmd("broadcast 1 bis add freq=1013")
    assert ok["cfg"] == "16_2" and ok["freq_actual"] == "1000.0"
    assert ha.cmd("broadcast 1 bis add freq=1013 exact=1", check=False).errno == -22
    assert ha.cmd("broadcast 1 bis add freq=7000 exact=1")["freq_actual"] == "7000.0"
    assert ha.cmd("broadcast 1 bis add freq=7300", check=False).errno == -34  # > 0.45 * 16 kHz
    assert ha.cmd("broadcast 1 bis add freq=50", check=False).errno == -34
    assert ha.cmd("broadcast 1 bis add level=-41", check=False).errno == -22
    assert ha.cmd("broadcast 1 bis add octets=77", check=False).errno == -95  # not a BAP config

    # 44.1 kHz grid: 44100 / 960 = 45.94 Hz
    ok = ha.cmd("broadcast 1 bis tone 1 freq=1000 level=-20")
    assert ok["freq_actual"] == "1000.0" and ok["level_actual"] == "-20.0"
    ha.cmd("broadcast 1 delete")
    ha.cmd("broadcast 1 create name=Sel")
    ha.cmd("broadcast 1 subgroup add preset=441_2")
    assert ha.cmd("broadcast 1 bis add freq=1000")["freq_actual"] == "1010.6"


def test_level_by_global_gain(pair):
    """Level steps of 0.71 dB by rewriting the global gain: measured peak level follows."""
    ha, dut = pair
    ha.cmd("broadcast 1 create name=Lvl preset=48_2 bis=1")
    ha.cmd("broadcast 1 bis tone 1 freq=1000 level=-6")
    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", timeout=15)

    m = dut.mark()
    dut.cmd("bcast scan name=Lvl")
    dut.wait_evt("dut_bcast_stream", since=m, state="started", timeout=SYNC_TIMEOUT)

    for level in (-6, -20, -30, -40, -2):
        actual = float(ha.cmd(f"broadcast 1 bis tone 1 freq=1000 level={level}")["level_actual"])
        assert abs(actual - level) <= 0.36
        m = dut.mark()
        dut.wait_evt("dut_audio", since=m, src="bcast_bis1", timeout=10)  # spans the change
        ev = dut.wait_evt("dut_audio", since=dut.mark(), src="bcast_bis1", timeout=10)
        assert abs(float(ev["level"]) - actual) <= 1.0, (level, ev)
        assert int(ev["clip"]) == 0, (level, ev)
        assert near(1000)(ev["freq"]), ev


@pytest.mark.parametrize(("cfg", "freq"), [("8_1", 133), ("16_2", 1000), ("48_2", 2000)])
def test_highest_level_is_not_clipped(pair, cfg, freq):
    """At the lower bitrates the decoder overshoots a full scale sine and clips it (heard as
    distortion), so the highest level is -2 dBFS: decoded at the hearing aid without a single
    sample at full scale, also for the table's worst tone (8_1, 133 Hz). 0 dBFS is refused."""
    ha, dut = pair
    ha.cmd(f"broadcast 1 create name=Max preset={cfg} bis=1")
    assert ha.cmd(f"broadcast 1 bis tone 1 freq={freq} level=0", check=False).errno == -22
    assert ha.cmd("limits")["level_max"] == "-2"
    ha.cmd(f"broadcast 1 bis tone 1 freq={freq} level=-2")
    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", timeout=15)

    m = dut.mark()
    dut.cmd("bcast scan name=Max")
    dut.wait_evt("dut_bcast_stream", since=m, state="started", timeout=SYNC_TIMEOUT)
    dut.wait_evt("dut_audio", since=m, src="bcast_bis1", timeout=10)  # spans the sync
    ev = dut.wait_evt("dut_audio", since=dut.mark(), src="bcast_bis1", timeout=10)
    assert int(ev["clip"]) == 0 and int(ev["lost"]) == 0, ev
    assert -3.5 <= float(ev["level"]) <= -0.5, ev


def test_mixed_frame_durations_and_rates(pair):
    """Two BIGs at once: 7.5 ms and 10 ms frames, each with BISes of different sampling rates.
    BIGs of different intervals collide every few events (the controller reserves about 1 ms per
    BIG event plus its air time, more than the 2.5 ms they could share), so the tool refuses
    unless --force, then
    warns; every stream still decodes with the right rate and tone."""
    ha, dut = pair
    ha.cmd("broadcast 1 create name=Short rtn=0")
    ha.cmd("broadcast 1 subgroup add preset=16_1")
    ha.cmd("broadcast 1 bis add freq=400")
    ha.cmd("broadcast 1 bis add freq=1000 rate=8 octets=26")  # 8_1
    ha.cmd("broadcast 2 create name=Long rtn=0")
    ha.cmd("broadcast 2 subgroup add preset=24_2")
    ha.cmd("broadcast 2 bis add freq=600")
    ha.cmd("broadcast 2 bis add freq=3000 rate=16 octets=40")  # 16_2
    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", big=1, timeout=15)

    rep = ha.cmd("broadcast 2 start", check=False)
    assert rep.errno == -33 and rep.raw.count("--force") == 1  # -EDOM
    ha.cmd("broadcast 2 start --force")
    warn = ha.wait_evt("broadcast_warning", big=2, timeout=15)
    assert warn["reason"] == "interval_mix" and warn["other_us"] == "7500"
    ha.wait_evt("broadcast_started", big=2, timeout=15)

    expect = {
        "Short": {1: (16000, 400), 2: (8000, 1000)},
        "Long": {1: (24000, 600), 2: (16000, 3000)},
    }
    for name, bises in expect.items():
        m = dut.mark()
        dut.cmd(f"bcast scan name={name}")
        for bis, (rate, freq) in bises.items():
            dut.wait_evt(
                "dut_bcast_stream",
                since=m,
                state="started",
                bis=str(bis),
                sample_rate=str(rate),
                timeout=SYNC_TIMEOUT,
            )
            dut.wait_evt(
                "dut_audio", since=m, src=f"bcast_bis{bis}", freq=near(freq, freq // 10), timeout=10
            )
        dut.cmd("bcast stop")
