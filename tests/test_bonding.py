# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Virtual devices against the simulated hearing aid: bonding, filling the bond list,
eviction, reconnection with stored keys and "phone forgot the device"."""

from helpers import connect, disconnect


def test_connect_and_bond(setup):
    ha, dut, addr = setup
    m = dut.mark()

    dev = connect(ha, addr, name="PhoneA")

    dut.wait_evt("dut_bonded", since=m, bonded=1)
    assert dut.cmd("bonds")["count"] == "1"
    listing = ha.cmd("bonding list")
    (entry,) = listing.data
    assert entry["id"] == str(dev) and entry["name"] == "PhoneA"
    assert entry["connection"] == "connected" and entry["security"] == "encrypted"
    assert entry["bonds"] == "1" and entry["bond"] == "present"
    show = ha.cmd("bonding show PhoneA --verbose")  # by name
    assert show["id"] == str(dev) and show["device"] == addr
    assert -100 < int(show["rssi"]) < 20  # read from the controller (127: unavailable)


def test_fill_bond_list_evicts_oldest(setup):
    ha, dut, addr = setup
    status = dut.cmd("status")
    max_bonds = int(status["max_bonds"])
    assert status["overwrite_oldest"] == "1"

    # One more device than the hearing aid can remember, bonded one after another
    devs = []
    for i in range(max_bonds + 1):
        m = dut.mark()
        dev = connect(ha, addr, name=f"phone{i}")
        dut.wait_evt("dut_bonded", since=m, bonded=1)
        disconnect(ha, dev)
        devs.append(dev)
        assert int(dut.cmd("bonds")["count"]) == min(i + 1, max_bonds)

    # The first (oldest) device was dropped: its stored key is rejected
    oldest = devs[0]
    ha.cmd(f"bonding connect {oldest} address={addr}")
    ha.wait_evt("bonding_connected", id=oldest)
    ha.cmd(f"bonding pair {oldest}")
    ev = ha.wait_evt("bonding_security_failed", id=oldest)
    assert ev["reason"] == "key_missing"
    disconnect(ha, oldest)

    # The newest one reconnects encrypted with its stored key, no new pairing
    newest = devs[-1]
    m = ha.mark()
    ha.cmd(f"bonding connect {newest} address={addr}")
    ev = ha.wait_evt("bonding_connected", id=newest)
    assert ev["bonded"] == "1"
    ha.cmd(f"bonding pair {newest}")
    ha.wait_evt("bonding_security", id=newest)
    assert not [e for e in ha.events_named("bonding_paired", since=m) if e["id"] == str(newest)]


def test_phone_forgets_device(setup):
    """Phone deletes its keys; the hearing aid keeps its bond and refuses to re-pair
    (Zephyr default: no unauthenticated bond overwrite) until its own bond is cleared."""
    ha, dut, addr = setup
    dev = connect(ha, addr)
    disconnect(ha, dev)
    phone_addr = ha.cmd("bonding list").data[0]["addr"]

    ha.cmd(f"bonding unpair {dev}")
    assert ha.cmd(f"bonding bonds {dev}")["count"] == "0"

    ha.cmd(f"bonding connect {dev} address={addr}")
    ha.wait_evt("bonding_connected", id=dev, bonded=0)
    ha.cmd(f"bonding pair {dev}")
    ha.wait_evt("bonding_pair_failed", id=dev)
    assert dut.cmd("bonds")["count"] == "1"
    disconnect(ha, dev)

    # Clear the bond on the hearing aid ("forget phone"), then pairing works again
    dut.cmd(f"unpair {phone_addr}")
    ha.cmd(f"bonding connect {dev} address={addr}")
    ha.wait_evt("bonding_connected", id=dev)
    ha.cmd(f"bonding pair {dev}")
    ha.wait_evt("bonding_paired", id=dev, bonded=1)


def test_parallel_connections(setup):
    """Several virtual devices connected to the hearing aid at the same time."""
    ha, dut, addr = setup
    a = connect(ha, addr, name="phone")
    b = connect(ha, addr, name="tablet")
    st = dut.cmd("status")
    assert st["conns"] == "2" and st["bonds"] == "2"
    assert ha.cmd("status --verbose")["conns"] == "2"

    disconnect(ha, a)
    assert dut.cmd("status")["conns"] == "1"
    assert ha.cmd("bonding list").data[1]["connection"] == "connected"
    disconnect(ha, b)


def test_quick_reconnect_cycles(setup):
    """Disconnect right after bonding, several devices in a row (regression: the DUT used to
    stall when its advertising was restarted on link release)."""
    ha, dut, addr = setup
    for i in range(4):
        dev = connect(ha, addr, name=f"p{i}")
        disconnect(ha, dev)
    st = dut.cmd("status")
    assert st["conns"] == "0" and st["bonds"] == "4"
