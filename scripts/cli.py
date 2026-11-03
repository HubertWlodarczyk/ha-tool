#!/usr/bin/env python3
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Talk to leabench on a real nRF5340 DK (or a simulated board's pty).

    scripts/cli.py --port /dev/ttyACM0 version
    scripts/cli.py --port /dev/ttyACM0 "dev create name=Phone" "dev list"
    scripts/cli.py --port /dev/ttyACM0 --events 10      # print EVT lines for 10 s

Each command prints its data lines and final OK/ERR; the exit code is 1 if a command failed.
"""

import argparse
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tests"))
from simlib import Device


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=1000000, help="0 for a pty (DK: 1000000)")
    ap.add_argument("--timeout", type=float, default=10)
    ap.add_argument("--events", type=float, default=0, help="seconds to print events afterwards")
    ap.add_argument("commands", nargs="*")
    args = ap.parse_args()

    log = pathlib.Path("/tmp") / "leabench_cli.log"
    dev = Device("leabench", args.port, log, baud=args.baud or None)
    failed = False
    try:
        dev.sync(timeout=5)
        for command in args.commands:
            rep = dev.cmd(command, timeout=args.timeout, check=False)
            for d in rep.data:
                print(d["_tag"], " ".join(f"{k}={v}" for k, v in d.items() if k != "_tag"))
            print(rep.raw)
            failed |= not rep.ok
        if args.events:
            seen = dev.mark()
            end = time.monotonic() + args.events
            while time.monotonic() < end:
                time.sleep(0.1)
                for ev in dev.events[seen:]:
                    print(ev.raw)
                seen = len(dev.events)
    finally:
        dev.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
