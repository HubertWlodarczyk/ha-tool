# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Phone simulation (LE Audio unicast + call / media / volume control) against the simulated
device. The DUT decodes the audio and reports the tone it hears, so each test checks the
audio content, not just the signalling."""

import pytest

from helpers import STREAM_TIMEOUT, connect, disconnect, near


@pytest.fixture
def phone(setup):
    ha, dut, addr = setup
    ha.cmd("mode unicast")
    dev = connect(ha, addr, name="phone")
    ha.cmd(f"le-source attach {dev}")
    return ha, dut


def start_music(ha, dut, freq=440, preset="48_2"):
    m = dut.mark()
    ha.cmd(f"le-source music start preset={preset} freq={freq}")
    ha.wait_evt("source_stream_open", ctx="media", preset=preset, timeout=STREAM_TIMEOUT)
    dut.wait_evt("dut_stream", since=m, state="started", dir="sink", ctx="media")
    dut.wait_evt("dut_audio", since=m, src="unicast", freq=near(freq), timeout=10)


def test_music(phone):
    ha, dut = phone
    m = dut.mark()
    start_music(ha, dut, freq=440, preset="48_2")

    started = dut.wait_evt("dut_stream", since=m, state="started", dir="sink")
    assert started["sample_rate"] == "48000"
    # 440 Hz is not on the 50 Hz grid of 48_2: the nearest tone (450 Hz) is sent
    dut.wait_evt("dut_media", since=0, state="playing", timeout=15)
    assert ha.cmd("le-source status")["ctx"] == "media"

    ha.cmd("le-source music stop")
    ha.wait_evt("source_stream_closed")
    dut.wait_evt("dut_stream", since=m, state="released", dir="sink")


def test_incoming_call_interrupts_music(phone):
    ha, dut = phone
    start_music(ha, dut, freq=440)

    # Ringing: same stream, context switched to Ringtone, ringtone audio
    m = dut.mark()
    ha.cmd("le-source call incoming uri=tel:5551234 name=Mum ring_freq=800 call_freq=300")
    ha.wait_evt("source_call", state="incoming")
    dut.wait_evt("dut_stream", since=m, state="metadata", ctx="ringtone", timeout=15)
    dut.wait_evt("dut_call", since=m, state="incoming", timeout=15)
    dut.wait_evt("dut_audio", since=m, src="unicast", freq=near(800), timeout=10)

    # The user answers on the device: bidirectional Conversational stream, call audio
    m = dut.mark()
    dut.cmd("call accept")
    ha.wait_evt("source_call", state="active", by="peer", timeout=15)
    ha.wait_evt("source_stream_open", ctx="conversational", bidir=1, timeout=STREAM_TIMEOUT)
    dut.wait_evt("dut_stream", since=m, state="started", dir="source", ctx="conversational")
    dut.wait_evt("dut_call", since=m, state="active")
    dut.wait_evt("dut_audio", since=m, src="unicast", freq=near(300), timeout=10)

    # Hang up on the phone: music resumes
    m = dut.mark()
    ha.cmd("le-source call end")
    ha.wait_evt("source_call", state="ended", by="local")
    dut.wait_evt("dut_call", since=m, state="ended", timeout=15)
    ha.wait_evt("source_stream_open", ctx="media", timeout=STREAM_TIMEOUT)
    dut.wait_evt("dut_audio", since=m, src="unicast", freq=near(440), timeout=STREAM_TIMEOUT)


def test_call_rejected_on_device(phone):
    ha, dut = phone
    m = dut.mark()
    ha.cmd("le-source call incoming uri=tel:999")
    ha.wait_evt("source_stream_open", ctx="ringtone", timeout=STREAM_TIMEOUT)
    dut.wait_evt("dut_call", since=m, state="incoming", timeout=15)

    dut.cmd("call end")
    ha.wait_evt("source_call", state="ended", by="peer", timeout=15)
    ha.wait_evt("source_stream_closed", timeout=15)
    assert ha.cmd("le-source status")["call"] == "-1"


def test_outgoing_call(phone):
    ha, dut = phone
    m = dut.mark()
    ha.cmd("le-source call outgoing uri=tel:112233 call_freq=350")
    ha.wait_evt("source_call", state="alerting")
    dut.wait_evt("dut_call", since=m, state=lambda s: s in ("dialing", "alerting"), timeout=30)

    ha.cmd("le-source call remote_answer")
    ha.wait_evt("source_stream_open", ctx="conversational", timeout=STREAM_TIMEOUT)
    dut.wait_evt("dut_call", since=m, state="active", timeout=15)
    dut.wait_evt("dut_audio", since=m, src="unicast", freq=near(350), timeout=STREAM_TIMEOUT)

    # Microphone stream from the device: received without loss
    st = ha.cmd("le-source status")
    assert int(st["rx"]) > 50 and st["rx_lost_pct"] == "0", st.raw

    ha.cmd("le-source call hold")
    dut.wait_evt("dut_call", since=m, state="locally_held", timeout=15)
    ha.cmd("le-source call retrieve")
    dut.wait_evt("dut_call", since=m, state="active", timeout=15)


def test_media_control_from_device(phone):
    ha, dut = phone
    start_music(ha, dut)
    dut.wait_evt("dut_mcp_discovered", since=0, err=0, timeout=15)

    m = dut.mark()
    dut.cmd("media pause")
    ha.wait_evt("source_music", state="paused", by="remote", timeout=15)
    ha.wait_evt("source_stream_closed", timeout=15)
    dut.wait_evt("dut_stream", since=m, state="released", dir="sink")

    dut.cmd("media play")
    ha.wait_evt("source_music", state="playing", by="remote", timeout=STREAM_TIMEOUT)
    dut.wait_evt("dut_audio", since=m, src="unicast", freq=near(440), timeout=STREAM_TIMEOUT)


def test_alert_during_music(phone):
    ha, dut = phone
    start_music(ha, dut)

    m = dut.mark()
    ha.cmd("le-source alert ms=2500 freq=1500")
    dut.wait_evt("dut_stream", since=m, state="metadata", ctx="alerts", timeout=15)
    dut.wait_evt("dut_audio", since=m, src="unicast", freq=near(1500, 75), timeout=10)
    dut.wait_evt("dut_stream", since=m, state="metadata", ctx="media", timeout=15)


def test_music_requested_during_alert_plays_after_it(phone):
    """Music started while an alert plays from idle is deferred, then plays when it ends."""
    ha, dut = phone
    ha.cmd("le-source alert ms=2500 freq=1500")
    ha.wait_evt("source_state", to="alert", timeout=STREAM_TIMEOUT)
    m = ha.mark()
    ha.cmd("le-source music start preset=48_2 freq=440")
    ha.wait_evt("source_music", since=m, state="deferred", timeout=5)
    ha.wait_evt("source_state", since=m, to="music", timeout=15)
    dut.wait_evt("dut_audio", since=dut.mark(), src="unicast", freq=near(440), timeout=10)


def test_volume_after_device_changed_it(setup):
    """The user turned the device's volume before the phone connected, so the server's
    change counter is no longer 0: the first write must still work (Zephyr retried with the stale
    counter and gave up, patch 0011; real devices are rarely at counter 0)."""
    ha, dut, addr = setup
    dut.cmd("volume 50")
    ha.cmd("mode unicast")
    dev = connect(ha, addr, name="phone")
    ha.cmd(f"le-source attach {dev}")
    m = dut.mark()
    ha.cmd("le-source volume set 120")
    dut.wait_evt("dut_volume", since=m, volume=120, timeout=STREAM_TIMEOUT)
    ha.wait_evt("source_volume", volume=120, timeout=15)


def test_reconnect_after_profile_discovery(phone):
    """Disconnect and connect the same identity again after a session with volume control.
    Zephyr's VOCS client kept its reference to the old connection (patch 0010), so the new
    connection from the same identity to the same peer failed with -EINVAL until reboot."""
    ha, dut = phone
    ha.cmd("le-source volume set 120")
    ha.wait_evt("source_volume", volume=120, timeout=STREAM_TIMEOUT)
    addr = dut.cmd("addr")["addr"]
    disconnect(ha, 1)
    m = ha.mark()
    ha.cmd(f"bonding connect 1 address={addr}")
    ha.wait_evt("bonding_connected", since=m, id=1, timeout=20)
    m = dut.mark()
    ha.cmd("le-source volume set 80")
    dut.wait_evt("dut_volume", since=m, volume=80, timeout=STREAM_TIMEOUT)


def test_volume(phone):
    ha, dut = phone
    m = dut.mark()
    ha.cmd("le-source volume set 120")
    dut.wait_evt("dut_volume", since=m, volume=120, timeout=STREAM_TIMEOUT)
    ha.wait_evt("source_volume", volume=120, timeout=15)

    ha.cmd("le-source mute on")
    dut.wait_evt("dut_volume", since=m, mute=1, timeout=15)


def test_state_machine(phone):
    """Every transition is reported; requests not allowed in the current state are refused."""
    ha, dut = phone
    assert ha.cmd("le-source status")["state"] == "idle"

    rep = ha.cmd("le-source call accept", check=False)  # nothing is ringing: refused at once
    assert rep.errno == -1 and "idle" in rep.raw and rep.values["hint"] == "le-source status"

    m = ha.mark()
    start_music(ha, dut)
    ha.wait_evt("source_state", since=m, to="music", cause="music_start")

    ha.cmd("le-source call incoming")
    ha.wait_evt("source_state", since=m, to="ringing", cause="call_incoming")
    ha.cmd("le-source music start")  # music deferred while ringing
    ha.wait_evt("source_music", since=m, state="deferred")

    ha.cmd("le-source call end")
    ha.wait_evt("source_state", since=m, to="music", cause="call_end", timeout=STREAM_TIMEOUT)
    st = ha.cmd("le-source status")
    assert st["state"] == "music" and st["call"] == "-1" and st["ctx"] == "media"


def test_argument_errors(tool):
    """Bad arguments are reported before anything else and change nothing (an idle board stays
    idle); text that does not fit a request field is refused instead of being cut."""
    bad = (
        "le-source preset bogus",
        "le-source mic gain x",
        "le-source balance set 999",
        "le-source alert ms=1",
    )
    for line in bad:
        assert tool.cmd(line, check=False).errno == -22, line
    assert tool.cmd("mode")["mode"] == "none"

    tool.cmd("mode broadcast")
    for line in bad:
        assert tool.cmd(line, check=False).errno == -22, line
    assert tool.cmd("le-source preset next", check=False).errno == -16  # valid, wrong mode

    tool.cmd("mode unicast")
    assert tool.cmd("le-source preset next", check=False).errno == -107  # nothing selected
    long = "x" * 32  # the fields hold 31 characters
    rep = tool.cmd(f"le-source call outgoing uri={long}", check=False)
    assert rep.errno == -22 and "uri max 31" in rep.raw
    assert tool.cmd(f"le-source call incoming name={long}", check=False).errno == -22


def test_link_lost_during_music(phone):
    """The device drops the link while streaming: reported, phone back to idle."""
    ha, dut = phone
    start_music(ha, dut)
    m = ha.mark()
    dut.cmd("disconnect")
    ha.wait_evt("source_link_lost", since=m, timeout=20)
    ha.wait_evt("source_state", since=m, to="idle", timeout=20)
    assert ha.cmd("le-source status")["stream"] == "stopped"


def test_call_from_device(phone):
    """The device user dials, holds and retrieves the call (CCP requests to the phone)."""
    ha, dut = phone
    dut.wait_evt("dut_ccp_discovered", since=0, timeout=STREAM_TIMEOUT)
    m = ha.mark()
    dut.cmd("call dial tel:5550000")
    ev = ha.wait_evt("source_call_request", since=m, op="originate", timeout=15)
    assert ev["uri"] == "tel:5550000"
    ha.wait_evt("source_state", since=m, to="dialing", cause="peer_originated", timeout=15)

    ha.cmd("le-source call remote_answer")
    ha.wait_evt("source_state", since=m, to="in_call", timeout=STREAM_TIMEOUT)
    dut.wait_evt("dut_call", state="active", timeout=15)

    m = ha.mark()
    dut.cmd("call hold")
    ha.wait_evt("source_state", since=m, to="held", cause="peer_held", timeout=15)
    m = ha.mark()
    dut.cmd("call retrieve")
    ha.wait_evt("source_state", since=m, to="in_call", cause="peer_retrieved", timeout=15)
