#!/usr/bin/env python3
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Interactive simulation: starts ha_tool and the simulated hearing aid in BabbleSim and prints
the pseudoterminals of their CLIs (connect with e.g. `screen /dev/pts/N`). Ctrl+C stops it.

    scripts/sim.py                 ha_tool + dut_sim
    scripts/sim.py --no-dut        ha_tool alone
    scripts/sim.py --speed 2       2x faster than real time
    scripts/sim.py --tcp 7000      also on TCP: ha_tool on 7000, dut on 7001 (Java / Python
                                   clients: HaTool.tcp("localhost", 7000))
"""

import argparse
import pathlib
import signal
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tests"))
from simlib import Sim, TcpBridge


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--no-dut", action="store_true", help="run ha_tool alone")
    parser.add_argument("--speed", type=float, default=1.0, help="simulated s per real s")
    parser.add_argument("--tcp", type=int, default=0, help="first TCP port (0: none)")
    parser.add_argument(
        "--out",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent / "tests/out/interactive",
    )
    args = parser.parse_args()

    images = [("ha_tool", "ha_tool")] + ([] if args.no_dut else [("dut", "dut_sim")])
    sim = Sim(args.out, speed=args.speed)
    try:
        sim.start(images, sim_length_s=24 * 3600, attach=False)
        bridges = []
        for i, (name, pty) in enumerate(sim.ptys):
            print(f"{name:8s} CLI: {pty}")
            if args.tcp:
                bridges.append(TcpBridge(pty, args.tcp + i))
                print(f"{name:8s} TCP: localhost:{args.tcp + i}")
        print(f"logs: {args.out}\nCtrl+C to stop")
        signal.pause()
    except KeyboardInterrupt:
        pass
    finally:
        sim.stop()


if __name__ == "__main__":
    main()
