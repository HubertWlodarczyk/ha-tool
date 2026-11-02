# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Pairing methods of the identities: passkey entry and numeric comparison, the tester acting
as the phone user ('bonding passkey', 'bonding confirm') and as the hearing aid user (dut)."""


def connect_dev(ha, addr, io):
    dev = ha.cmd(f"bonding create io={io}").int("id")
    ha.cmd(f"bonding connect {dev} address={addr}")
    ha.wait_evt("bonding_connected", id=dev, timeout=20)
    return dev


def test_passkey_entry(setup):
    ha, dut, addr = setup
    dut.cmd("io display")  # hearing aid shows the passkey
    dev = connect_dev(ha, addr, "keyboard")  # phone user types it
    m = dut.mark()
    ha.cmd(f"bonding pair {dev} sec=3")
    passkey = dut.wait_evt("dut_passkey", since=m, timeout=20)["passkey"]
    ha.wait_evt("bonding_passkey_request", id=dev, timeout=10)
    ha.cmd(f"bonding passkey {dev} {passkey}")
    ev = ha.wait_evt("bonding_security", id=dev, timeout=20)
    assert int(ev["level"]) >= 3  # authenticated (LE Secure Connections: 4)
    ha.wait_evt("bonding_paired", id=dev, bonded=1)


def test_wrong_passkey(setup):
    ha, dut, addr = setup
    dut.cmd("io display")
    dev = connect_dev(ha, addr, "keyboard")
    m = dut.mark()
    ha.cmd(f"bonding pair {dev} sec=3")
    passkey = int(dut.wait_evt("dut_passkey", since=m, timeout=20)["passkey"])
    ha.wait_evt("bonding_passkey_request", id=dev, timeout=10)
    ha.cmd(f"bonding passkey {dev} {(passkey + 1) % 1000000}")
    assert ha.wait_evt("bonding_pair_failed", id=dev, timeout=20)["reason"] in (
        "auth_fail",
        "unspecified",
    )
    assert dut.cmd("bonds")["count"] == "0"


def test_numeric_comparison(setup):
    ha, dut, addr = setup
    dut.cmd("io yesno")
    dev = connect_dev(ha, addr, "yesno")
    m = dut.mark()
    ha.cmd(f"bonding pair {dev} sec=4")
    phone_code = ha.wait_evt("bonding_confirm", id=dev, timeout=20)["passkey"]
    assert dut.wait_evt("dut_confirm", since=m, timeout=20)["passkey"] == phone_code
    ha.cmd(f"bonding confirm {dev} yes")
    dut.cmd("confirm yes")
    assert ha.wait_evt("bonding_security", id=dev, timeout=20)["level"] == "4"


def test_numeric_comparison_rejected_on_hearing_aid(setup):
    ha, dut, addr = setup
    dut.cmd("io yesno")
    dev = connect_dev(ha, addr, "yesno")
    m = dut.mark()
    ha.cmd(f"bonding pair {dev} sec=4")
    ha.wait_evt("bonding_confirm", id=dev, timeout=20)
    dut.wait_evt("dut_confirm", since=m, timeout=20)
    ha.cmd(f"bonding confirm {dev} yes")
    dut.cmd("confirm no")
    ha.wait_evt("bonding_pair_failed", id=dev, timeout=20)
    assert dut.cmd("bonds")["count"] == "0"


def test_authenticated_pairing_impossible_without_io(setup):
    """A phone asking for an authenticated link from a hearing aid without IO fails."""
    ha, _dut, addr = setup
    dev = connect_dev(ha, addr, "keyboard")  # hearing aid: no IO (default)
    ha.cmd(f"bonding pair {dev} sec=3")
    ev = ha.wait_evt("bonding_security_failed", id=dev, timeout=20)
    assert ev["reason"] in ("auth_requirement", "auth_fail")


def test_passkey_shown_by_phone(setup):
    """The phone shows the passkey (bonding_passkey), the hearing aid user types it."""
    ha, dut, addr = setup
    dut.cmd("io keyboard")
    dev = connect_dev(ha, addr, "display")
    m = dut.mark()
    ha.cmd(f"bonding pair {dev} sec=3")
    passkey = ha.wait_evt("bonding_passkey", id=dev, timeout=20)["passkey"]
    dut.wait_evt("dut_passkey_request", since=m, timeout=10)
    dut.cmd(f"passkey {passkey}")
    ha.wait_evt("bonding_paired", id=dev, bonded=1, timeout=20)


def test_pairing_cancelled_by_disconnection(setup):
    """The link drops while the hearing aid user is typing the passkey: reported, no bond."""
    ha, dut, addr = setup
    dut.cmd("io keyboard")
    dev = connect_dev(ha, addr, "display")
    m = dut.mark()
    ha.cmd(f"bonding pair {dev} sec=3")
    ha.wait_evt("bonding_passkey", id=dev, timeout=20)
    dut.wait_evt("dut_passkey_request", since=m, timeout=10)
    mh = ha.mark()
    dut.cmd("disconnect")
    ha.wait_evt("bonding_pair_failed", since=mh, id=dev, timeout=20)
    ha.wait_evt("bonding_disconnected", since=mh, id=dev, timeout=20)
    assert next(d for d in ha.cmd("bonding list").data if d["id"] == str(dev))["bonds"] == "0"
