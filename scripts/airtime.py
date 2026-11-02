#!/usr/bin/env python3
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Air-time analysis of a BabbleSim run: who transmitted what, when, and why ISO events are lost.

    scripts/airtime.py <results dir or sim id> [--from-ms N] [--to-ms N] [--dev 0]

Run a test with HA_PHY_ARGS=-dump_imm so the phy writes complete traces to
$BSIM_OUT_PATH/results/<sim_id>/d_2G4_<dev>.Txv2.csv.

Streams are told apart by access address and classified from their timing:
  adv   0x8E89BED6 (legacy / extended advertising)
  conn  transmitted by both devices: ACL (interval >= 7.5 ms, sparse) or CIS (dense, ISO interval)
  bis   transmitted by one device every ISO interval
  pa    transmitted by one device, sparse (periodic advertising)
"""

from __future__ import annotations

import argparse
import csv
import os
import pathlib
import statistics
import sys
from collections import defaultdict

ADV_AA = 0x8E89BED6


def load_tx(path: pathlib.Path) -> list[tuple[int, int, int, int]]:
    """(start_us, end_us, access_address, payload_len) per transmitted packet."""
    out = []
    if not path.exists():
        return out
    with open(path) as f:
        reader = csv.reader(f)
        next(reader, None)
        for row in reader:
            if len(row) < 12:
                continue
            try:
                out.append((int(row[2]), int(row[3]), int(row[5], 16), int(row[11])))
            except ValueError:
                continue
    return out


def events(starts: list[int], gap_us: int = 2500) -> list[int]:
    """Groups packets into events (bursts): returns the start time of each event."""
    ev = []
    last = None
    for t in starts:
        if last is None or t - last > gap_us:
            ev.append(t)
        last = t
    return ev


def classify(per_dev: dict[int, dict[int, list]], dev: int) -> dict[int, str]:
    kinds = {}
    for aa, pkts in per_dev[dev].items():
        if aa == ADV_AA:
            kinds[aa] = "adv"
            continue
        shared = any(aa in per_dev[d] for d in per_dev if d != dev)
        ev = events(sorted(p[0] for p in pkts))
        spacing = statistics.median(b - a for a, b in zip(ev, ev[1:])) if len(ev) > 2 else 0
        if shared:
            kinds[aa] = "cis" if 0 < spacing < 20000 else "acl"
        else:
            kinds[aa] = "bis" if 0 < spacing < 20000 else "pa"
    return kinds


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("run", help="results directory or sim id")
    ap.add_argument("--from-ms", type=float, default=0)
    ap.add_argument("--to-ms", type=float, default=1e12)
    ap.add_argument("--dev", type=int, default=0, help="device whose schedule is analysed")
    args = ap.parse_args()

    run = pathlib.Path(args.run)
    if not run.is_dir():
        run = (
            pathlib.Path(
                os.environ.get("BSIM_OUT_PATH", pathlib.Path.home() / "zephyrproject/tools/bsim")
            )
            / "results"
            / args.run
        )
    lo, hi = args.from_ms * 1000, args.to_ms * 1000

    per_dev: dict[int, dict[int, list]] = {}
    for f in sorted(run.glob("d_2G4_*.Txv2.csv")):
        d = int(f.name.split("_")[2].split(".")[0])
        streams = defaultdict(list)
        for p in load_tx(f):
            if lo <= p[0] <= hi:
                streams[p[2]].append(p)
        if streams:
            per_dev[d] = streams

    if args.dev not in per_dev:
        sys.exit(f"no transmissions of device {args.dev} in {run}")

    kinds = classify(per_dev, args.dev)
    window_us = max(p[1] for s in per_dev[args.dev].values() for p in s) - min(
        p[0] for s in per_dev[args.dev].values() for p in s
    )

    print(f"Device {args.dev}, window {window_us / 1000:.0f} ms")
    print(
        f"{'stream':6} {'access addr':12} {'pkts':>6} {'events':>6} {'interval':>9} "
        f"{'pkt us':>7} {'event us':>9} {'airtime':>8}"
    )
    busy = []
    for aa, pkts in sorted(per_dev[args.dev].items(), key=lambda kv: kinds[kv[0]]):
        starts = sorted(p[0] for p in pkts)
        ev = events(starts)
        spacing = statistics.median(b - a for a, b in zip(ev, ev[1:])) if len(ev) > 2 else 0
        dur = statistics.median(p[1] - p[0] for p in pkts)
        # event length: first packet start to last packet end inside the event
        ev_len = []
        i = 0
        for e in ev:
            last_end = e
            while i < len(pkts) and sorted(pkts)[i][0] < e + 2500:
                last_end = sorted(pkts)[i][1]
                i += 1
            ev_len.append(last_end - e)
        air = sum(p[1] - p[0] for p in pkts) / window_us * 100
        print(
            f"{kinds[aa]:6} 0x{aa:08X}   {len(pkts):6} {len(ev):6} {spacing / 1000:8.2f}ms "
            f"{dur:7.0f} {statistics.median(ev_len) if ev_len else 0:9.0f} {air:7.1f}%"
        )
        busy += [(p[0], p[1], kinds[aa], aa) for p in pkts]

    # Lost CIS events: expected every ISO interval, check what occupied the radio instead
    for aa in [a for a, k in kinds.items() if k == "cis"]:
        ev = events(sorted(p[0] for p in per_dev[args.dev][aa]))
        if len(ev) < 3:
            continue
        interval = min(b - a for a, b in zip(ev, ev[1:]) if b - a > 1000)
        expected = round((ev[-1] - ev[0]) / interval) + 1
        print(
            f"\nCIS 0x{aa:08X}: {len(ev)} of {expected} events transmitted "
            f"({100 - len(ev) / expected * 100:.0f}% lost), ISO interval {interval / 1000:.2f} ms"
        )
        blockers = defaultdict(int)
        busy.sort()
        anchor0 = ev[0]
        have = {round((e - anchor0) / interval) for e in ev}
        for n in range(expected):
            if n in have:
                continue
            t = anchor0 + n * interval
            hit = {(k, a) for s, e, k, a in busy if s < t + 2000 and e > t - 300 and a != aa}
            for k, a in hit:
                blockers[f"{k} 0x{a:08X}"] += 1
            if not hit:
                blockers["nothing on air (skipped)"] += 1
        for name, cnt in sorted(blockers.items(), key=lambda kv: -kv[1]):
            print(f"  missing event overlaps {name}: {cnt}")


if __name__ == "__main__":
    main()
