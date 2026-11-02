# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Fixtures: simulated boards, or a real DK with --hw-port."""

import pathlib
import re
import sys

import pytest

from helpers import OUT, ROOT
from simlib import Device, Sim

# The Python client of the repository (tests/test_clients.py), without installing it
sys.path.insert(0, str(ROOT / "clients/python"))


def pytest_addoption(parser):
    parser.addoption(
        "--hw-port",
        default=None,
        help="serial port of a real nRF5340 DK running ha_tool (e.g. /dev/ttyACM0); "
        "tests needing the simulated hearing aid are skipped",
    )
    parser.addoption("--hw-baud", type=int, default=1000000)


def pytest_sessionfinish(session, exitstatus):
    """Protocol conformance of everything ha_tool answered in this session (simlib)."""
    from simlib import conformance

    if not conformance.replies and not conformance.events:
        return
    errors, info = conformance.report()
    report = OUT / "conformance.txt"
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text("\n".join(["errors:", *errors, "", *info]) + "\n")
    tr = session.config.pluginmanager.get_plugin("terminalreporter")
    if tr is not None:
        tr.write_line(f"protocol conformance: {len(errors)} error(s), see {report}")
        for line in errors[:10]:
            tr.write_line("  " + line[:200])
    if errors and session.exitstatus == 0:
        session.exitstatus = 1


def _test_dir(request) -> pathlib.Path:
    return OUT / re.sub(r"[^A-Za-z0-9_.-]", "_", request.node.name)


@pytest.fixture
def sim(request):
    if request.config.getoption("hw_port"):
        pytest.skip("needs the simulation (simulated hearing aid)")
    s = Sim(_test_dir(request))
    yield s
    s.stop()


@pytest.fixture
def tool(request) -> Device:
    """ha_tool alone: simulated, or the real DK given with --hw-port (reset before the test)."""
    port = request.config.getoption("hw_port")
    if port:
        ha = Device(
            "ha_tool",
            port,
            _test_dir(request) / "ha_tool.log",
            baud=request.config.getoption("hw_baud"),
            app="ha_tool",
        )
        ha.sync()
        ha.cmd("reset", timeout=15)
        yield ha
        ha.close()
        return
    s = Sim(_test_dir(request))
    (ha,) = s.start([("ha_tool", "ha_tool")])
    yield ha
    s.stop()


@pytest.fixture
def pair(sim) -> tuple[Device, Device]:
    """ha_tool + simulated hearing aid, not connected (broadcast tests)."""
    ha, dut = sim.start([("ha_tool", "ha_tool"), ("dut", "dut_sim")])
    return ha, dut


@pytest.fixture
def setup(sim) -> tuple[Device, Device, str]:
    """ha_tool + simulated hearing aid; returns (ha, dut, dut_address)."""
    ha, dut = sim.start([("ha_tool", "ha_tool"), ("dut", "dut_sim")])
    addr = dut.cmd("addr")["addr"]
    return ha, dut, addr
