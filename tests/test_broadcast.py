# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Auracast broadcast source against the simulated hearing aid's broadcast sink. Each BIS
carries its own tone, so the DUT can verify left/right routing."""

import time

import pytest

from helpers import SYNC_TIMEOUT, near, start_broadcast


def test_stereo_broadcast_left_right(pair):
    ha, dut = pair
    start_broadcast(ha, "name=Hall preset=24_2 bis=2 lang=pol program=Lecture id=0xABCDEF")

    m = dut.mark()
    dut.cmd("bcast scan name=Hall")
    found = dut.wait_evt("dut_bcast_found", since=m, timeout=SYNC_TIMEOUT)
    assert found["broadcast_id"] == "0xABCDEF"
    assert found["pbp"] == "1" and found["pbp_sq"] == "1" and found["pbp_encrypted"] == "0"
    dut.wait_evt("dut_bcast_base", since=m, bis_count=2, timeout=SYNC_TIMEOUT)
    dut.wait_evt("dut_bcast_stream", since=m, state="started", bis=2, timeout=SYNC_TIMEOUT)

    dut.wait_evt("dut_audio", since=m, src="bcast_bis1", freq=near(500), timeout=10)
    dut.wait_evt("dut_audio", since=m, src="bcast_bis2", freq=near(1000), timeout=10)

    # Tones come from the frame table: exact tones of the 50 Hz grid of 24_2
    for b in [d for d in ha.cmd("broadcast 1 show").data if d["_tag"] == "BIS"]:
        assert b["cfg"] == "24_2" and float(b["freq_actual"]) == float(b["tone"]), b

    # Live tone change on the left channel
    m = dut.mark()
    ha.cmd("broadcast 1 bis tone 1 freq=700")
    dut.wait_evt("dut_audio", since=m, src="bcast_bis1", freq=near(700), timeout=10)

    # Source stops: the sink loses the BIG
    m = dut.mark()
    ha.cmd("broadcast 1 stop")
    ha.wait_evt("broadcast_stopped", timeout=15)
    dut.wait_evt("dut_bcast_stream", since=m, state="stopped", timeout=SYNC_TIMEOUT)


def test_encrypted_broadcast(pair):
    ha, dut = pair
    start_broadcast(ha, "name=Private preset=16_2 bis=1 code=Secret123", tones=(600,))

    # No code: the sink sees an encrypted BIG and cannot sync
    m = dut.mark()
    dut.cmd("bcast scan name=Private")
    found = dut.wait_evt("dut_bcast_found", since=m, timeout=SYNC_TIMEOUT)
    assert found["pbp_encrypted"] == "1"
    dut.wait_evt("dut_bcast_biginfo", since=m, encrypted=1, timeout=SYNC_TIMEOUT)
    dut.wait_evt("dut_bcast_error", since=m, reason="code_required", timeout=SYNC_TIMEOUT)

    # Wrong code: BIG sync fails
    m = dut.mark()
    dut.cmd("bcast scan name=Private code=wrong")
    dut.wait_evt("dut_bcast_stopped", since=m, timeout=SYNC_TIMEOUT)

    # Right code: audio
    m = dut.mark()
    dut.cmd("bcast scan name=Private code=Secret123")
    dut.wait_evt("dut_bcast_stream", since=m, state="started", bis=1, timeout=SYNC_TIMEOUT)
    dut.wait_evt("dut_audio", since=m, src="bcast_bis1", freq=near(600), timeout=10)


def test_high_quality_mono(pair):
    ha, dut = pair
    start_broadcast(ha, "name=Music preset=48_2 bis=1 pbp=hq", tones=(1200,))

    m = dut.mark()
    dut.cmd("bcast scan name=Music")
    found = dut.wait_evt("dut_bcast_found", since=m, timeout=SYNC_TIMEOUT)
    assert found["pbp_hq"] == "1"
    dut.wait_evt("dut_audio", since=m, src="bcast_bis1", freq=near(1200), timeout=SYNC_TIMEOUT)


def test_scan_ignores_other_broadcasts(pair):
    ha, dut = pair
    start_broadcast(ha, "name=Airport bis=1", tones=(400,))

    m = dut.mark()
    dut.cmd("bcast scan name=Cinema")
    with pytest.raises(TimeoutError):
        dut.wait_evt("dut_bcast_found", since=m, timeout=5)
    assert dut.cmd("bcast status")["state"] == "scanning"


# --- Auracast builder: BIG / subgroups / BISes composed by the tester ---


def test_custom_subgroups_and_per_bis_parameters(pair):
    """One BIG, two subgroups (languages, presets), three BISes with their own tone, location
    and, for one of them, a different sampling rate and frame size."""
    ha, dut = pair
    ha.cmd("broadcast 1 create name=Conference id=0x111111")
    assert ha.cmd("broadcast 1 subgroup add preset=24_2 lang=pol")["sg"] == "1"
    assert ha.cmd("broadcast 1 subgroup add preset=16_2 lang=eng ctx=live")["sg"] == "2"
    ha.cmd("broadcast 1 bis add subgroup=1 location=left freq=500")
    ok = ha.cmd("broadcast 1 bis add subgroup=1 location=right freq=1000 rate=48 octets=100")
    assert ok["cfg"] == "48_2" and ok["freq_actual"] == "1000.0"
    # Only LC3 configurations of the frame table: 16 kHz with the subgroup's 60 octets is none
    assert ha.cmd("broadcast 1 bis add subgroup=1 rate=16", check=False).errno == -95
    ha.cmd("broadcast 1 bis add subgroup=2 freq=1500")

    show = ha.cmd("broadcast 1 show")
    assert show["bis"] == "3" and show["subgroups"] == "2" and show["fits"] == "1"
    assert show["sdu"] == "100"
    bis = [d for d in show.data if d["_tag"] == "BIS"]
    assert [b["rate"] for b in bis] == ["24000", "48000", "16000"]
    assert [b["cfg"] for b in bis] == ["24_2", "48_2", "16_2"]
    assert [b["location"] for b in bis] == ["left", "right", "mono"]

    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", bis=3, subgroups=2, timeout=15)

    m = dut.mark()
    dut.cmd("bcast scan name=Conference")
    dut.wait_evt("dut_bcast_base", since=m, bis_count=3, subgroups=2, timeout=SYNC_TIMEOUT)
    expect = {
        "1": dict(sg="1", sample_rate="24000", octets="60", lang="pol"),
        "2": dict(sg="1", sample_rate="48000", octets="100", lang="pol"),
        "3": dict(sg="2", sample_rate="16000", octets="40", lang="eng"),
    }
    for idx, values in expect.items():
        dut.wait_evt(
            "dut_bcast_stream", since=m, state="started", bis=idx, **values, timeout=SYNC_TIMEOUT
        )

    for idx, freq in (("1", 500), ("2", 1000), ("3", 1500)):
        dut.wait_evt("dut_audio", since=m, src=f"bcast_bis{idx}", freq=near(freq), timeout=10)

    # A hearing aid picking only the English BIS
    m = dut.mark()
    dut.cmd("bcast scan name=Conference bis=3")
    dut.wait_evt(
        "dut_bcast_stream", since=m, state="started", bis=3, lang="eng", timeout=SYNC_TIMEOUT
    )
    dut.wait_evt("dut_audio", since=m, src="bcast_bis3", freq=near(1500), timeout=10)
    assert dut.cmd("bcast status")["streams"] == "1"


def test_two_broadcasts_at_once(pair):
    """Different BIG-level parameters (encryption, preset) need separate BIGs."""
    ha, dut = pair
    ha.cmd("broadcast 1 create name=Open preset=16_2 bis=1")
    ha.cmd("broadcast 1 bis tone 1 freq=600")
    ha.cmd("broadcast 2 create name=Locked preset=24_2 bis=2 code=pass1234")
    ha.cmd("broadcast 1 start")
    ha.cmd("broadcast 2 start")
    ha.wait_evt("broadcast_started", since=0, big=1, timeout=15)
    ha.wait_evt("broadcast_started", since=0, big=2, encrypted=1, timeout=15)
    assert ha.cmd("broadcast list")["count"] == "2"

    m = dut.mark()
    dut.cmd("bcast scan name=Open")
    dut.wait_evt("dut_audio", since=m, src="bcast_bis1", freq=near(600), timeout=SYNC_TIMEOUT)

    m = dut.mark()
    dut.cmd("bcast scan name=Locked code=pass1234")
    dut.wait_evt("dut_audio", since=m, src="bcast_bis1", freq=near(500), timeout=SYNC_TIMEOUT)
    dut.wait_evt("dut_audio", since=m, src="bcast_bis2", freq=near(1000), timeout=SYNC_TIMEOUT)

    ha.cmd("broadcast 2 stop")
    assert ha.cmd("broadcast 1 show")["state"] == "active"


def test_started_big_never_overlaps_running_one(pair):
    """A BIG started next to a running one gets its own air time, whatever the start phase.

    The controller looked only at the next event of each running role when placing a new BIG:
    behind a periodic advertising train it could land exactly on a later event of a running BIG
    of the same interval; both then lost every other event. The periodic advertising train of
    the new broadcast could likewise land on the running BIG (Zephyr patch 0006). The start
    phase follows the moment the command arrives, so the test restarts the second BIG many
    times while the hearing aid listens to the first.
    """
    ha, dut = pair
    ha.cmd("broadcast 1 create name=Open preset=16_2 bis=1")
    ha.cmd("broadcast 2 create name=Other preset=24_2 bis=2")
    ha.cmd("broadcast 1 start")
    m = dut.mark()
    dut.cmd("bcast scan name=Open")
    dut.wait_evt("dut_audio", since=m, src="bcast_bis1", timeout=SYNC_TIMEOUT)

    for i in range(30):
        time.sleep((i * 7 % 61) / 1000)  # vary the start phase
        ha.cmd("broadcast 2 start")
        ha.wait_evt("broadcast_started", big=2, timeout=15)
        dut.wait_evt("dut_audio", since=dut.mark(), src="bcast_bis1", timeout=5)  # spans start
        ev = dut.wait_evt("dut_audio", since=dut.mark(), src="bcast_bis1", timeout=5)
        assert ev["lost"] == "0", (i, ev)
        ha.cmd("broadcast 2 stop")
        ha.wait_evt("broadcast_stopped", big=2, timeout=15)


def test_airtime_limit_and_rtn_override(pair):
    ha, dut = pair
    ha.cmd("broadcast 1 create name=Heavy preset=48_6 bis=3")
    assert ha.cmd("broadcast 1 show")["fits"] == "0"
    rep = ha.cmd("broadcast 1 start", check=False)
    assert not rep.ok and rep.errno == -7 and "not enough air time" in rep.raw  # -E2BIG

    ha.cmd("broadcast 1 set rtn=1")
    assert ha.cmd("broadcast 1 show")["fits"] == "1"
    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", bis=3, timeout=15)

    m = dut.mark()
    dut.cmd("bcast scan name=Heavy")
    for idx, freq in (("1", 500), ("2", 1000), ("3", 1500)):
        dut.wait_evt(
            "dut_audio", since=m, src=f"bcast_bis{idx}", freq=near(freq), timeout=SYNC_TIMEOUT
        )


def test_builder_validation(pair):
    ha, _ = pair
    ha.cmd("broadcast 1 create name=Draft")
    assert not ha.cmd("broadcast 1 bis add subgroup=1", check=False).ok  # no subgroup yet
    ha.cmd("broadcast 1 subgroup add preset=16_2")
    assert not ha.cmd("broadcast 1 start", check=False).ok  # no BIS yet
    ha.cmd("broadcast 1 bis add")
    ha.cmd("broadcast 1 set interval=7.5")
    assert not ha.cmd("broadcast 1 start", check=False).ok  # 10 ms frames vs 7.5 ms BIG
    ha.cmd("broadcast 1 set interval=10")
    ha.cmd("broadcast 1 start")
    assert not ha.cmd("broadcast 1 bis add", check=False).ok  # streaming: stop first
    ha.cmd("broadcast 1 stop")
    ha.cmd("broadcast 1 bis add freq=900")  # draft kept, editable
    assert ha.cmd("broadcast 1 show")["bis"] == "2"


def test_air_budget_over_broadcasts(pair):
    """The air time budget covers all broadcasts: two BIGs of 8 do not fit together (each one
    alone does)."""
    ha, _ = pair
    ha.cmd("broadcast 1 create name=A preset=16_2 bis=8 rtn=0")
    ha.cmd("broadcast 2 create name=B preset=16_2 bis=8 rtn=0")
    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", big=1, timeout=15)
    rep = ha.cmd("broadcast 2 start", check=False)
    assert not rep.ok and rep.errno == -7 and "not enough air time" in rep.raw
    ha.cmd("broadcast 1 stop")
    ha.cmd("broadcast 2 start")  # alone: fine
    ha.wait_evt("broadcast_started", big=2, timeout=15)
