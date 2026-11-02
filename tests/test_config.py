# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Consistency between the application and the controller image (separate builds, so no
BUILD_ASSERT can tie them): broadcast limits, ISO buffers, SDU size. No simulation needed."""

import re

from helpers import ROOT


def kconfig_defaults(path) -> dict[str, int]:
    """'config X' ... 'default N' of a Kconfig file."""
    out, name = {}, None
    for line in path.read_text().splitlines():
        m = re.match(r"config (\w+)", line)
        if m:
            name = m.group(1)
        m = re.match(r"\s+default (\d+)", line)
        if m and name and name not in out:
            out[name] = int(m.group(1))
    return out


def conf(path) -> dict[str, int]:
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r"CONFIG_(\w+)=(\d+)$", line.strip())
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


APP = kconfig_defaults(ROOT / "ha_tool/Kconfig")
APP_CONF = conf(ROOT / "ha_tool/prj.conf")
CTLR = conf(ROOT / "ha_tool/netcore.conf")


def test_broadcast_limits_match_controller():
    assert APP["HA_BCAST_MAX_BIGS"] <= CTLR["BT_CTLR_ADV_ISO_SET"]
    assert APP["HA_BCAST_MAX_BIS"] <= CTLR["BT_CTLR_ADV_ISO_STREAM_MAX"]
    assert APP["HA_BCAST_MAX_BIS_TOTAL"] <= CTLR["BT_CTLR_ADV_ISO_STREAM_COUNT"]
    assert CTLR["BT_CTLR_ISOAL_SOURCES"] >= CTLR["BT_CTLR_ADV_ISO_STREAM_COUNT"]
    # one advertising set per BIG
    assert APP_CONF["BT_EXT_ADV_MAX_ADV_SET"] >= APP["HA_BCAST_MAX_BIGS"]
    assert CTLR["BT_CTLR_ADV_SET"] >= APP["HA_BCAST_MAX_BIGS"]


def test_iso_buffers_match():
    # The host flow control trusts the controller's buffer count (audio_tx.c relies on it)
    assert APP_CONF["BT_ISO_TX_BUF_COUNT"] == CTLR["BT_CTLR_ISO_TX_BUFFERS"]


def test_sdu_fits_controller():
    sdu = APP_CONF["BT_ISO_TX_MTU"]
    assert sdu <= CTLR["BT_CTLR_ISO_TX_SDU_LEN_MAX"] <= CTLR["BT_CTLR_ISO_TX_BUFFER_SIZE"]
    assert sdu <= CTLR["BT_CTLR_ADV_ISO_PDU_LEN_MAX"]
