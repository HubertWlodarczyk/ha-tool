# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Device controls a companion app uses: presets (HAS), microphone (MICP + AICS) and the
volume offset / balance (VOCS)."""

import pytest

from helpers import connect

TIMEOUT = 45  # first use discovers the services


@pytest.fixture
def phone(setup):
    ha, dut, addr = setup
    ha.cmd("mode unicast")
    dev = connect(ha, addr, name="app")
    ha.cmd(f"le-source attach {dev}")
    return ha, dut


def test_presets(phone):
    ha, dut = phone
    ha.cmd("le-source preset list")
    disc = ha.wait_evt("source_discovered", timeout=TIMEOUT)
    assert disc["has"] == "1" and disc["micp"] == "1" and disc["vocs"] == "1"
    assert ha.wait_evt("source_preset_list", timeout=15)["count"] == "3"
    names = [e["name"] for e in ha.events_named("source_preset_record")]
    assert names == ["Universal", "Restaurant", "Music"]

    m = dut.mark()
    ha.cmd("le-source preset set 2")
    dut.wait_evt("dut_preset", since=m, active=2, name="Restaurant", timeout=15)
    ha.wait_evt("source_preset", active=2, timeout=15)

    ha.cmd("le-source preset next")
    dut.wait_evt("dut_preset", since=m, active=3, timeout=15)


def test_microphone(phone):
    ha, dut = phone
    m = dut.mark()
    ha.cmd("le-source mic mute on")
    dut.wait_evt("dut_mic", since=m, mute=1, timeout=TIMEOUT)
    ha.wait_evt("source_mic", mute=1, timeout=15)

    ha.cmd("le-source mic gain -10")
    dut.wait_evt("dut_mic_input", since=m, gain=-10, timeout=15)
    ha.wait_evt("source_mic_input", gain=-10, timeout=15)


def test_balance(phone):
    ha, dut = phone
    m = dut.mark()
    ha.cmd("le-source balance set -40")
    dut.wait_evt("dut_balance", since=m, offset=-40, timeout=TIMEOUT)
    ha.wait_evt("source_balance", offset=-40, timeout=15)
