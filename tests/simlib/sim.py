# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""BabbleSim simulations of leabench / dut_sim images (device processes, phy, handbrake)."""

from __future__ import annotations

import os
import pathlib
import pty
import re
import shutil
import signal
import subprocess
import threading
import uuid

from .device import Device

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = pathlib.Path(os.environ.get("LEA_BUILD_DIR", ROOT / "build"))
BSIM_OUT = pathlib.Path(
    os.environ.get("BSIM_OUT_PATH", pathlib.Path.home() / "zephyrproject/tools/bsim")
)
# Build variant: "" (normal), "asan" (scripts/build.sh sim --sanitize), "cov" (--coverage)
VARIANT = os.environ.get("LEA_VARIANT", "")
SIM_SPEED = float(os.environ.get("LEA_SIM_SPEED", "1.0"))  # simulated seconds per real second
PTY_RE = re.compile(r"UART 0 .*connected to pseudotty: (\S+)")
# Sanitizer reports in a device log (ASan aborts the process, UBSan only prints)
SANITIZER_RE = re.compile(
    r"ERROR: (Address|Leak)Sanitizer|runtime error: |SUMMARY: UndefinedBehaviorSanitizer"
)


class SanitizerError(AssertionError):
    """A device image built with --sanitize reported an error (see its log)."""


def _sanitizer_env() -> dict[str, str]:
    env = dict(os.environ)
    supp = pathlib.Path(__file__).resolve().parents[1] / "sanitizer.supp"
    env.setdefault("UBSAN_OPTIONS", f"print_stacktrace=1:color=never:suppressions={supp}")
    # No LeakSanitizer: the firmware never calls the host malloc() (Zephyr heaps are static
    # arrays), so leaks reported at process exit are host libc / simulator ones
    env.setdefault("ASAN_OPTIONS", "color=never:detect_leaks=0")
    return env


def image_dir(app: str) -> pathlib.Path:
    """Build directory of an image in the selected variant."""
    return BUILD / (f"{app}_{VARIANT}" if VARIANT else app)


class Sim:
    """A BabbleSim simulation with N devices (+ a handbrake to run at real-time pace)."""

    def __init__(
        self, outdir: pathlib.Path, speed: float = SIM_SPEED, flash_from: pathlib.Path | None = None
    ):
        """Each device keeps its flash in <outdir>/<name>.flash. A new simulation starts with
        erased flash, or with the flash files of a previous run (flash_from): like rebooting
        the boards."""
        self.sim_id = "ha_" + uuid.uuid4().hex[:8]
        self.outdir = outdir
        self.outdir.mkdir(parents=True, exist_ok=True)
        for old in self.outdir.glob("*.flash"):
            old.unlink()
        if flash_from is not None:
            for f in pathlib.Path(flash_from).glob("*.flash"):
                shutil.copy(f, self.outdir / f.name)
        self.speed = speed
        self.procs: list[subprocess.Popen] = []
        self.devices: list[Device] = []
        self.names: list[str] = []

    def _spawn(self, args: list[str], log_name: str):
        log_path = self.outdir / f"{log_name}.log"
        # stdout on a pty so the simulator line-buffers its output
        master, slave = pty.openpty()
        proc = subprocess.Popen(
            args,
            cwd=BSIM_OUT / "bin",
            env=_sanitizer_env(),
            stdin=subprocess.DEVNULL,
            stdout=slave,
            stderr=slave,
            start_new_session=True,
        )
        os.close(slave)
        self.procs.append(proc)
        found: dict[str, str] = {}
        ready = threading.Event()

        def pump():
            pending = ""
            with open(log_path, "wb") as log:
                while True:
                    try:
                        chunk = os.read(master, 4096)
                    except OSError:
                        break
                    if not chunk:
                        break
                    log.write(chunk)
                    log.flush()
                    if not ready.is_set():
                        pending += chunk.decode(errors="replace")
                        m = PTY_RE.search(pending)
                        if m:
                            found["pty"] = m.group(1)
                            ready.set()
            os.close(master)
            ready.set()

        threading.Thread(target=pump, daemon=True).start()

        def wait_pty() -> str:
            ready.wait(15)
            if "pty" not in found:
                raise RuntimeError(f"{log_name}: no pty announced, see {log_path}")
            return found["pty"]

        return wait_pty

    def start(
        self, images: list[tuple[str, str]], sim_length_s: float = 3600, attach: bool = True
    ) -> list[Device]:
        """images: (name, app) pairs, app being the build directory name (leabench/dut_sim).

        With attach=False the CLIs are left free for interactive use and self.ptys holds the
        (name, pty) pairs.
        """
        n = len(images)
        waiters = []
        # Devices block until the phy connects, so start everything before waiting for ptys
        for d, (name, app) in enumerate(images):
            exe = image_dir(app) / "zephyr" / "zephyr.exe"
            if not exe.exists():
                hint = {"asan": " sim --sanitize", "cov": " sim --coverage"}.get(VARIANT, "")
                raise FileNotFoundError(f"{exe} missing, run scripts/build.sh{hint}")
            args = [
                str(exe),
                f"-s={self.sim_id}",
                f"-d={d}",
                "-RealEncryption=1",
                "-uart0_pty",
                f"-rs={d + 1}",
                f"-flash_app={self.outdir / (name + '.flash')}",
            ]
            if os.environ.get("LEA_GDB") == name:
                # Debug aid: SIGINT the inferior to get all thread backtraces in <name>.log
                args = [
                    "gdb",
                    "-batch",
                    "-ex",
                    "handle SIGPIPE nostop noprint",
                    "-ex",
                    "run",
                    "-ex",
                    "thread apply all bt 20",
                    "--args",
                    *args,
                ]
            self.names.append(name)
            waiters.append((name, self._spawn(args, name)))
        self._spawn(
            ["./bs_device_handbrake", f"-s={self.sim_id}", f"-d={n}", f"-r={self.speed}"],
            "handbrake",
        )
        # LEA_PHY_ARGS: extra phy options, e.g. "-dump_imm" to keep complete air traces in
        # $BSIM_OUT_PATH/results/<sim_id>/ (analyse with scripts/airtime.py)
        self._spawn(
            [
                "./bs_2G4_phy_v1",
                f"-s={self.sim_id}",
                f"-D={n + 1}",
                f"-sim_length={int(sim_length_s * 1e6)}",
                *os.environ.get("LEA_PHY_ARGS", "").split(),
            ],
            "phy",
        )
        self.ptys = [(name, wait_pty()) for name, wait_pty in waiters]
        if not attach:
            return []
        for name, pty_path in self.ptys:
            app = dict(images)[name]
            self.devices.append(Device(name, pty_path, self.outdir / f"{name}.log", app=app))
        for dev in self.devices:
            dev.sync()
        return self.devices

    def stop(self) -> None:
        for dev in self.devices:
            dev.close()
        for proc in reversed(self.procs):
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        for proc in self.procs:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
        self.check_sanitizers()

    def check_sanitizers(self) -> None:
        """Fails if a device log holds a sanitizer report (images built with --sanitize)."""
        found = []
        for name in self.names:
            log = self.outdir / f"{name}.log"
            if not log.exists():
                continue
            for line in log.read_text(errors="replace").splitlines():
                if SANITIZER_RE.search(line):
                    found.append(f"{name}: {line.strip()}")
        if found:
            raise SanitizerError("sanitizer reports:\n  " + "\n  ".join(found[:20]))
