# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Virtual devices connect with resolvable private addresses (RPA) like real phones: the
device sees an RPA, resolves it to the right identity after bonding, and every reconnection uses
a fresh RPA. Several identities bonded with the same device must never be confused."""

from helpers import connect, disconnect


def is_rpa(addr: str) -> bool:
    return addr.endswith("/random") and 0x40 <= int(addr[:2], 16) <= 0x7F


def test_rpa_and_resolution(setup):
    ha, dut, addr = setup
    a = ha.cmd("bonding create PhoneA")
    identity = a["addr"]
    dev = a.int("id")

    m = dut.mark()
    ha.cmd(f"bonding connect {dev} address={addr}")
    first = ha.wait_evt("bonding_connected", id=dev, timeout=20)["local"]
    assert is_rpa(first) and first != identity
    assert dut.wait_evt("dut_connected", since=m)["peer"] == first  # not resolvable yet

    ha.cmd(f"bonding pair {dev}")
    ha.wait_evt("bonding_paired", id=dev, bonded=1, timeout=20)
    assert [b["peer"] for b in dut.cmd("bonds").data] == [identity]  # IRK distributed
    disconnect(ha, dev)

    # Reconnection: new RPA, resolved by the device to the bonded identity
    m = dut.mark()
    ha.cmd(f"bonding connect {dev} address={addr}")
    second = ha.wait_evt("bonding_connected", id=dev, timeout=20)["local"]
    assert is_rpa(second) and second != first
    ev = dut.wait_evt("dut_connected", since=m)
    assert ev["peer"] == identity and ev["bonded"] == "1"


def test_identities_not_confused(setup):
    """Two virtual devices bonded with the same device each resolve to their own identity."""
    ha, dut, addr = setup
    ids = {}
    for name in ("PhoneA", "PhoneB"):
        dev = connect(ha, addr, name=name)
        disconnect(ha, dev)
        ids[dev] = next(d["addr"] for d in ha.cmd("bonding list").data if d["id"] == str(dev))
    assert dut.cmd("bonds")["count"] == "2"

    for dev in list(ids) + list(ids):
        m = dut.mark()
        ha.cmd(f"bonding connect {dev} address={addr}")
        ha.wait_evt("bonding_connected", id=dev, timeout=20)
        ha.cmd(f"bonding pair {dev}")
        ha.wait_evt("bonding_security", id=dev, timeout=20)
        assert dut.wait_evt("dut_connected", since=m)["peer"] == ids[dev]
        disconnect(ha, dev)
