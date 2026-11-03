#!/usr/bin/env python3
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Static worst-case stack use of the DK images (the simulation cannot show stack overflows:
in BabbleSim the threads run on host stacks).

Build with per-function stack usage first:
    scripts/build.sh dk -- -DEXTRA_CFLAGS=-fstack-usage -Dhci_ipc_EXTRA_CFLAGS=-fstack-usage \
        -Dhci_ipc_CONFIG_LTO=n      (or let this script do it: --build)
Then:
    scripts/stack_report.py [--build] [--dir build/su] [--top 25]

For every function: its own frame (GCC .su) and the deepest chain of direct calls below it
(arm objdump of zephyr.elf). Calls through pointers (Bluetooth callbacks, work handlers, shell
handlers) cannot be followed statically, so each such entry point is its own row: the stack a
thread needs is its entry's chain plus the worst callback it dispatches. The report lists the
thread entries with their stack size and the deepest callbacks of this project.
"""

import argparse
import collections
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OBJDUMP = "arm-zephyr-eabi-objdump"

# Thread entries and the Kconfig / source value of their stack (app core, net core)
THREADS = {
    "app": [
        ("shell_thread", "CONFIG_SHELL_STACK_SIZE"),
        ("phone_thread", 4096),
        ("evt_thread", 3072),
        ("tx_thread", 1024),
        ("bg_thread_main", "CONFIG_MAIN_STACK_SIZE"),
        ("work_queue_main", "CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE"),
        ("rx_work_handler", "CONFIG_BT_RX_STACK_SIZE"),
    ],
    "net": [
        ("bg_thread_main", "CONFIG_MAIN_STACK_SIZE"),
        ("work_queue_main", "CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE"),
        ("tx_thread", "CONFIG_BT_HCI_TX_STACK_SIZE"),
        ("recv_thread", "CONFIG_BT_CTLR_RX_STACK_SIZE"),
    ],
}


def objdump_tool(build: pathlib.Path) -> str:
    cache = build / "leabench" / "CMakeCache.txt"
    for line in cache.read_text().splitlines() if cache.exists() else []:
        if line.startswith("CMAKE_OBJDUMP:"):
            return line.split("=", 1)[1]
    return OBJDUMP


def load_su(image: pathlib.Path) -> dict[str, tuple[int, str]]:
    frames: dict[str, tuple[int, str]] = {}
    for su in image.rglob("*.su"):
        for line in su.read_text(errors="replace").splitlines():
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            name = parts[0].rsplit(":", 1)[-1]
            size = int(parts[1])
            if size >= frames.get(name, (0, ""))[0]:
                frames[name] = (size, parts[2])
    return frames


def call_graph(elf: pathlib.Path, objdump: str) -> dict[str, set[str]]:
    out = subprocess.run([objdump, "-d", str(elf)], capture_output=True, text=True).stdout
    graph: dict[str, set[str]] = collections.defaultdict(set)
    func = None
    head = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")
    call = re.compile(r"\s(?:bl|blx|b\.w|b)\s+[0-9a-f]+ <([^>+]+)(?:\+0x[0-9a-f]+)?>")
    for line in out.splitlines():
        m = head.match(line)
        if m:
            func = m.group(1)
            graph.setdefault(func, set())
            continue
        if func:
            m = call.search(line)
            if m and m.group(1) != func:
                graph[func].add(m.group(1))
    return graph


def strip_suffix(name: str) -> str:
    return re.sub(r"\.(constprop|isra|part|lto_priv)\.\d+.*$", "", name)


def worst(graph, frames):
    memo: dict[str, tuple[int, list[str]]] = {}

    def depth(f: str, stack: set[str]) -> tuple[int, list[str]]:
        if f in memo:
            return memo[f]
        own = frames.get(strip_suffix(f), (0, ""))[0]
        best, path = 0, []
        stack.add(f)
        for g in graph.get(f, ()):
            if g in stack:
                continue
            d, p = depth(g, stack)
            if d > best:
                best, path = d, p
        stack.discard(f)
        memo[f] = (own + best, [f, *path])
        return memo[f]

    return depth


def kconfig(image: pathlib.Path, key):
    if isinstance(key, int):
        return key
    cfg = image / "zephyr" / ".config"
    for line in cfg.read_text().splitlines():
        if line.startswith(key + "="):
            return int(line.split("=", 1)[1])
    return 0


def report(build: pathlib.Path, top: int) -> None:
    objdump = objdump_tool(build)
    for core, image_dir in (("app", "leabench"), ("net", "hci_ipc")):
        image = build / image_dir
        frames = load_su(image)
        graph = call_graph(image / "zephyr" / "zephyr.elf", objdump)
        depth = worst(graph, frames)
        print(f"\n=== {core} core ({image_dir}): {len(frames)} functions with a frame")
        print("thread entry                 stack   worst direct chain (+ callbacks on top)")
        for entry, key in THREADS[core]:
            size = kconfig(image, key)
            names = [f for f in graph if strip_suffix(f) == entry]
            if not names:
                print(f"{entry:28} {size:6}   (not in this image)")
                continue
            d, path = depth(names[0], set())
            print(f"{entry:28} {size:6}   {d:5}  {' > '.join(strip_suffix(p) for p in path[:6])}")

        print(f"\nlargest frames ({core}):")
        for name, (size, kind) in sorted(frames.items(), key=lambda kv: -kv[1][0])[:top]:
            print(f"  {size:6}  {kind:8} {name}")
        print(f"\ndeepest chains ({core}, any function):")
        rows = sorted(((depth(f, set()), f) for f in graph), key=lambda r: -r[0][0])
        for (d, path), _f in rows[:top]:
            print(f"  {d:6}  {' > '.join(strip_suffix(p) for p in path[:7])}")


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--dir", type=pathlib.Path, default=ROOT / "build" / "su")
    ap.add_argument("--build", action="store_true", help="build the DK images with -fstack-usage")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()
    if args.build:
        subprocess.run(
            [
                "west",
                "build",
                "-b",
                "nrf5340dk/nrf5340/cpuapp",
                "--sysbuild",
                "-p",
                "auto",
                "-d",
                str(args.dir),
                str(ROOT / "leabench"),
                "--",
                "-DEXTRA_CFLAGS=-fstack-usage",
                "-Dhci_ipc_EXTRA_CFLAGS=-fstack-usage",
                "-Dhci_ipc_CONFIG_LTO=n",
            ],
            check=True,
        )
    if not (args.dir / "leabench" / "zephyr" / "zephyr.elf").exists():
        sys.exit(f"no build in {args.dir}: run with --build")
    report(args.dir, args.top)


if __name__ == "__main__":
    main()
