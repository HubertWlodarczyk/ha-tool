# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Link parameters: connection interval, latency, supervision timeout, PHY, data length, and the
answer to the device's own parameter requests."""


def connected(ha, addr, extra=""):
    dev = ha.cmd("bonding create").int("id")
    ha.cmd(f"bonding connect {dev} address={addr} {extra}".strip())
    ha.wait_evt("bonding_connected", id=dev, timeout=20)
    return dev


def entry(ha, dev):
    return ha.cmd(f"bonding show {dev} --verbose")


def test_connect_with_parameters(setup):
    ha, _, addr = setup
    dev = connected(ha, addr, "interval=30 latency=2 sto=2000")
    e = entry(ha, dev)
    assert (e["interval_us"], e["latency"], e["sto_ms"]) == ("30000", "2", "2000")


def test_parameter_update_from_tool(setup):
    ha, dut, addr = setup
    dev = connected(ha, addr)
    m = dut.mark()
    ha.cmd(f"bonding param {dev} interval=100 sto=5000")
    ha.wait_evt("bonding_param_updated", id=dev, interval_us=100000, sto_ms=5000, timeout=20)
    dut.wait_evt("dut_param_updated", since=m, interval_us=100000, timeout=20)


def test_phy_and_data_length(setup):
    ha, dut, addr = setup
    dev = connected(ha, addr)
    m = dut.mark()
    ha.cmd(f"bonding phy {dev} coded")
    ha.wait_evt("bonding_phy_updated", id=dev, tx="coded", rx="coded", timeout=20)
    dut.wait_evt("dut_phy_updated", since=m, tx="coded", timeout=20)
    assert entry(ha, dev)["phy"] == "coded"
    ha.cmd(f"bonding phy {dev} 2m")
    ha.wait_evt("bonding_phy_updated", id=dev, tx="2m", timeout=20)

    ha.cmd(f"bonding dle {dev} 100")
    ha.wait_evt("bonding_dle_updated", id=dev, tx_len=100, timeout=20)


def test_device_parameter_request(setup):
    ha, dut, addr = setup
    dev = connected(ha, addr, "interval=50")

    ha.cmd("bonding policy param_req=reject")
    dut.cmd("conn param interval=100 sto=4000")
    assert ha.wait_evt("bonding_param_req", id=dev, timeout=20)["accepted"] == "0"
    assert entry(ha, dev)["interval_us"] == "50000"

    ha.cmd("bonding policy param_req=accept")
    m = ha.mark()
    dut.cmd("conn param interval=100 sto=4000")
    ha.wait_evt("bonding_param_req", since=m, id=dev, accepted=1, timeout=20)
    ha.wait_evt("bonding_param_updated", since=m, id=dev, interval_us=100000, timeout=20)
