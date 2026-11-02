# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Test support: BabbleSim simulations and the text CLI client of a board."""

from .bridge import TcpBridge
from .device import CliError, Device, Event, Reply, parse_kv, split_line
from .sim import BSIM_OUT, BUILD, ROOT, SIM_SPEED, Sim

__all__ = [
    "BSIM_OUT",
    "BUILD",
    "ROOT",
    "SIM_SPEED",
    "CliError",
    "Device",
    "Event",
    "Reply",
    "Sim",
    "TcpBridge",
    "parse_kv",
    "split_line",
]
