# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""ha_tool never writes flash: virtual devices, their identities and bonds live in RAM. After a
reboot of the tool the device list is empty, while the hearing aid (which does persist its bonds)
keeps an orphaned bond; new virtual devices bond normally."""

import time

from helpers import OUT, connect, disconnect
from simlib import Sim

IMAGES = [("ha_tool", "ha_tool"), ("dut", "dut_sim")]


def test_tool_reboot():
    run1, run2 = OUT / "test_tool_reboot_run1", OUT / "test_tool_reboot_run2"

    sim = Sim(run1)
    try:
        ha, dut = sim.start(IMAGES)
        addr = dut.cmd("addr")["addr"]
        phone = connect(ha, addr, name="Phone")
        phone_addr = ha.cmd("bonding list").data[0]["addr"]
        disconnect(ha, phone)
        ha.cmd("bonding create Spare")
        assert ha.cmd("status --verbose")["identities"] == "2"
    finally:
        sim.stop()

    # "Reboot" both boards: new simulation, same flash contents
    sim = Sim(run2, flash_from=run1)
    try:
        ha, dut = sim.start(IMAGES)
        assert dut.cmd("addr")["addr"] == addr
        assert dut.cmd("bonds")["count"] == "1"  # hearing aid: orphaned bond survives

        assert ha.cmd("status --verbose")["identities"] == "0"  # tool: nothing was stored
        assert not ha.cmd("bonding list").data

        # The old identity address without its keys: the hearing aid refuses to overwrite the
        # bond (what a phone sees after a factory reset). Given explicitly here; note that the
        # simulation's entropy is deterministic, so a plain 'bonding create' after a reboot would
        # also repeat the first address, unlike the real board.
        old = ha.cmd(f"bonding create Old addr={phone_addr.split('/')[0]}").int("id")
        ha.cmd(f"bonding connect {old} address={addr}")
        ha.wait_evt("bonding_connected", id=old, timeout=20)
        m = ha.mark()
        ha.cmd(f"bonding pair {old}")
        # Refused either during pairing, or after the key distribution when the hearing aid
        # finds the identity address already bonded with other keys (disconnect, auth failure)
        deadline = time.monotonic() + 20
        while not ha.events_named("bonding_pair_failed", since=m) and not ha.events_named(
            "bonding_disconnected", since=m
        ):
            assert time.monotonic() < deadline, "old identity was not refused"
            time.sleep(0.2)
        assert dut.cmd("bonds")["count"] == "1"
        if not ha.events_named("bonding_disconnected", since=m):
            disconnect(ha, old)

        # A new identity bonds normally next to the orphaned entry
        connect(ha, addr, name="New")
        assert dut.cmd("bonds")["count"] == "2"
    finally:
        sim.stop()
