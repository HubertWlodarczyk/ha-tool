# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Client libraries against the simulated board: the Python package (pty and TCP) and the Java
library (TCP, skipped without a JDK)."""

import os
import pathlib
import shutil
import subprocess

import pytest

from hatool import HaTool, HaToolError
from helpers import OUT, ROOT, near
from simlib import Device, Sim, TcpBridge


@pytest.fixture
def board(request):
    """ha_tool for the client under test (pty free), dut_sim through simlib."""
    out = OUT / request.node.name.replace("[", "_").replace("]", "_")
    sim = Sim(out)
    sim.start([("ha_tool", "ha_tool"), ("dut", "dut_sim")], attach=False)
    ptys = dict(sim.ptys)
    dut = Device("dut", ptys["dut"], out / "dut.log", app="dut_sim")
    dut.sync()
    sim.devices.append(dut)
    yield sim, ptys["ha_tool"], dut
    sim.stop()


@pytest.fixture
def client_log(board):
    """Log callback of the client under test: <test out>/client.log."""
    sim, _, _ = board
    with open(sim.outdir / "client.log", "w", buffering=1) as f:
        yield lambda line: f.write(line + "\n")


@pytest.mark.parametrize("transport", ["pty", "tcp"])
def test_python_client(board, transport, client_log):
    _sim, pty, dut = board
    bridge = None
    if transport == "pty":
        tool = HaTool.open(pty, log=client_log)
    else:
        bridge = TcpBridge(pty, 0)
        tool = HaTool.tcp("127.0.0.1", bridge.port, log=client_log)
    try:
        assert tool.version().proto == 1
        assert tool.limits().bigs == 3
        assert len(tool.limits().rows("LC3")) == 16

        # Identity with a name containing spaces, typed reply values
        addr = dut.cmd("addr")["addr"]
        d = tool.bonding.create("Zoë phone")
        assert isinstance(d.id, int) and d.name == "Zoë phone"
        tool.bonding.connect(d.id, address=addr)
        ev = tool.wait_event("bonding_connected", id=d.id, timeout=20)
        assert ev.bonded is False and ev.peer == addr
        tool.bonding.pair(d.id)
        tool.wait_event("bonding_paired", id=d.id, bonded=True, timeout=20)
        rows = tool.bonding.list().rows("IDENTITY")
        assert rows[0]["name"] == "Zoë phone" and rows[0]["bonds"] == 1

        # Errors: unknown key on the board, bad enum and unknown argument in the client
        with pytest.raises(HaToolError) as e:
            tool.raw("bonding create frq=1")
        assert e.value.errno == -22 and e.value.fields["key"] == "frq"
        with pytest.raises(HaToolError) as e:
            tool.raw("nosuchcommand")
        assert e.value.errno == -22
        with pytest.raises(ValueError):
            tool.bonding.create(io="loud")
        with pytest.raises(TypeError):
            tool.bonding.create(nmae="x")

        # Event filter
        tool.evt.off("mode")
        assert tool.evt.list().off == "mode"
        tool.evt.on()

        # Broadcast with a spaced name; the hearing aid hears the tone
        tool.mode("broadcast")
        tool.broadcast.create(1, name="Conference hall", preset="24_2", bis=1)
        tone = tool.broadcast.bis.tone(1, 1, freq=1013, level=-20)
        assert tone.freq_actual == 1000.0 and tone.level_actual == -20.0
        events = tool.subscribe(timeout=15)
        tool.broadcast.start(1)
        assert next(e for e in events if e.name == "broadcast_started").bis == 1
        m = dut.mark()
        dut.cmd('bcast scan name="Conference hall"')
        dut.wait_evt("dut_audio", since=m, src="bcast_bis1", freq=near(1000), timeout=30)
        assert tool.time().us > 0
    finally:
        tool.close()
        if bridge:
            bridge.close()


def java_home() -> str | None:
    if os.environ.get("JAVA_HOME"):
        return os.environ["JAVA_HOME"]
    local = sorted(pathlib.Path.home().glob(".local/opt/jdk-*"))
    if local:
        return str(local[-1])
    java = shutil.which("java")
    return str(pathlib.Path(java).resolve().parent.parent) if java else None


def test_java_client(request):
    """clients/java: ./gradlew test with its SimulationTest against the board over TCP."""
    home = java_home()
    if home is None:
        pytest.skip("no JDK (JAVA_HOME, ~/.local/opt/jdk-*, java on PATH)")
    out = OUT / request.node.name
    sim = Sim(out)
    try:
        sim.start([("ha_tool", "ha_tool")], attach=False)
        bridge = TcpBridge(dict(sim.ptys)["ha_tool"], 0)
        env = dict(os.environ, JAVA_HOME=home, HATOOL_TCP=f"127.0.0.1:{bridge.port}")
        res = subprocess.run(
            ["./gradlew", "test", "--rerun-tasks", "-q", "--console=plain"],
            cwd=ROOT / "clients/java",
            env=env,
            capture_output=True,
            text=True,
            timeout=600,
        )
        (out / "gradle.log").write_text(res.stdout + res.stderr)
        assert res.returncode == 0, res.stdout[-3000:] + res.stderr[-3000:]
        report = ROOT / "clients/java/build/test-results/test/TEST-io.hatool.SimulationTest.xml"
        head = report.read_text().split(">", 2)[1]
        assert 'tests="1"' in head and 'skipped="0"' in head and 'failures="0"' in head, head
        bridge.close()
    finally:
        sim.stop()
