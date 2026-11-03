# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""TX power as one more dimension of variation (no connectors needed): per virtual device
connection and per broadcast; the simulated device sees the RSSI follow."""

from helpers import connect


def dut_rssi(dut) -> int:
    links = dut.cmd("rssi").data
    assert len(links) == 1
    return int(links[0]["rssi"])


def wait_rssi(dut, cond, tries=20):
    import time

    for _ in range(tries):
        v = dut_rssi(dut)
        if cond(v):
            return v
        time.sleep(0.2)
    raise AssertionError(f"RSSI {v}")


def test_connection_txpower(setup):
    ha, dut, addr = setup
    dev = connect(ha, addr, pair=False)
    before = wait_rssi(dut, lambda v: v != 127)
    assert ha.cmd(f"bonding txpower {dev} -20")["txpower"] == "-20"
    after = wait_rssi(dut, lambda v: v <= before - 15)
    assert ha.cmd(f"bonding txpower {dev} 99", check=False).errno == -22
    print("rssi", before, after)


def test_broadcast_txpower(setup):
    ha, dut, _ = setup
    ha.cmd("broadcast 1 create name=Loud preset=16_2 bis=1")
    ha.cmd("broadcast 2 create name=Quiet preset=16_2 bis=1 rtn=0")
    assert ha.cmd("broadcast 2 txpower -20")["applied"] == "start"
    ha.cmd("broadcast 1 start")
    ha.wait_evt("broadcast_started", big=1, timeout=15)
    ha.cmd("broadcast 2 start")
    ha.wait_evt("broadcast_started", big=2, timeout=15)

    rssi = {}
    for name in ("Loud", "Quiet"):
        m = dut.mark()
        dut.cmd(f"bcast scan name={name}")
        rssi[name] = int(dut.wait_evt("dut_bcast_found", since=m, timeout=30)["rssi"])
        dut.wait_evt("dut_audio", since=m, src="bcast_bis1", timeout=30)  # BIG still received
        dut.cmd("bcast stop")
    assert rssi["Quiet"] <= rssi["Loud"] - 15, rssi

    # Live change on a streaming broadcast
    assert ha.cmd("broadcast 1 txpower -8")["txpower"] == "-8"
