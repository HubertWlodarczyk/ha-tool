#!/usr/bin/env python3
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Radio capacity in BabbleSim: what the air time budget predicts next to what a receiver
actually gets.

For every broadcast configuration (preset, rtn, PHY, encryption) it asks the budget how many
BISes one BIG may have, then starts a BIG with that many (expected lossless) and one more with
--force (past the budget), and lets the simulated hearing aid receive each for a few seconds:
found or not, frames lost per BIS. Multi-BIG cases start several BIGs at their combined edge.
Unicast cases stream to the hearing aid while other links (other identities connected to a
second simulated hearing aid) run with given connection intervals, and count lost CIS frames.

    scripts/capacity.py broadcast [--jobs 4] [--quick]   -> build/capacity/broadcast.csv
    scripts/capacity.py multi     [--jobs 4]             -> build/capacity/multi.csv
    scripts/capacity.py unicast   [--jobs 4]             -> build/capacity/unicast.csv
    scripts/capacity.py accept                           -> build/capacity/accept.csv
      which BIGs the controller itself creates (no reception): every preset, rtn, PHY,
      encryption and BIS count started with --force, next to the budget's airtime_us / fits

Verdicts: ok (as predicted), conservative (the budget refused a configuration that was received
without loss), FALSE_FIT (the budget accepted a configuration that lost frames).
"""

import argparse
import concurrent.futures
import csv
import itertools
import pathlib
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))

from helpers import near  # noqa: E402
from simlib.sim import Sim  # noqa: E402

OUT = ROOT / "build" / "capacity"
MEASURE_S = 4  # seconds of dut_audio reports per BIG (one report per second)
SYNC_TIMEOUT = 30


ACTUAL: dict[int, list[int]] = {}  # tones actually sent, per BIG (this process)


def tones(big: int, n: int) -> list[int]:
    return [300 + 100 * (8 * (big - 1) + i) for i in range(n)]


def create(ha, big: int, cfg: dict, n: int) -> dict:
    code = f" code=Cap{big}Key" if cfg.get("enc") else ""
    ha.cmd(
        f"broadcast {big} create name=Cap{big} preset={cfg['preset']} bis={n} rtn={cfg['rtn']}"
        f" phy={cfg['phy']}{code}" + (f" iso={cfg['iso']}" if cfg.get("iso") else "")
    )
    actual = []
    for i, f in enumerate(tones(big, n), start=1):
        # the nearest tone of the configuration's grid (7.5 ms: 66.7 Hz, 44.1 kHz: 45.9 Hz steps)
        actual.append(round(float(ha.cmd(f"broadcast {big} bis tone {i} freq={f}")["freq_actual"])))
    ACTUAL[big] = actual
    return ha.cmd(f"broadcast {big} show").values


def receive(dut, big: int, n: int, enc: bool) -> tuple[bool, int, int]:
    """(found, frames lost, frames received) over MEASURE_S seconds of every BIS."""
    m = dut.mark()
    dut.cmd(f"bcast scan name=Cap{big}" + (f" code=Cap{big}Key" if enc else ""))
    try:
        dut.wait_evt("dut_audio", since=m, src=f"bcast_bis{n}", timeout=SYNC_TIMEOUT)
    except TimeoutError:
        dut.cmd("bcast stop", check=False)
        return False, 0, 0
    lost = frames = 0
    for _ in range(MEASURE_S):
        m = dut.mark()
        for i, f in enumerate(ACTUAL.get(big, tones(big, n)), start=1):
            try:
                ev = dut.wait_evt("dut_audio", since=m, src=f"bcast_bis{i}", timeout=10)
            except TimeoutError:
                lost += 100
                continue
            lost += int(ev["lost"]) + (0 if near(f)(ev["freq"]) else int(ev["frames"]))
            frames += int(ev["frames"])
    dut.cmd("bcast stop", check=False)
    return True, lost, frames


def run_bigs(name: str, bigs: list[tuple[dict, int]], force: bool) -> dict:
    """Starts the BIGs (with --force if asked), receives each; one result row."""
    sim = Sim(OUT / "runs" / name)
    ha, dut = sim.start([("ha_tool", "ha_tool"), ("dut", "dut_sim")])
    row = {"case": name, "force": int(force)}
    try:
        air = []
        for b, (cfg, n) in enumerate(bigs, start=1):
            show = create(ha, b, cfg, n)
            air.append(show["airtime_us"])
        row["airtime_us"] = "+".join(air)
        refused = None
        for b in range(1, len(bigs) + 1):
            rep = ha.cmd(f"broadcast {b} start" + (" --force" if force else ""), check=False)
            if not rep.ok:
                refused = f"BIG {b}: {rep.errno}"
                break
            ha.wait_evt("broadcast_started", big=b, timeout=15)
        row["model"] = "refused " + refused if refused else ("forced" if force else "fits")
        if refused:
            return row
        time.sleep(1)
        found_all, lost_all, frames_all = True, 0, 0
        for b, (cfg, n) in enumerate(bigs, start=1):
            found, lost, frames = receive(dut, b, n, bool(cfg.get("enc")))
            found_all &= found
            lost_all += lost
            frames_all += frames
        row["found"] = int(found_all)
        row["lost"] = lost_all
        row["frames"] = frames_all
        row["loss_pct"] = round(100 * lost_all / max(frames_all + lost_all, 1), 2)
        row["errors"] = len(ha.events_named("broadcast_error"))
    finally:
        sim.stop()
    return row


def budget_max(cfg: dict) -> int:
    """BISes one BIG of this configuration may have, by the budget (fresh tool)."""
    sim = Sim(OUT / "runs" / f"budget-{label(cfg)}")
    (ha,) = sim.start([("ha_tool", "ha_tool")])
    try:
        ha.cmd("mode broadcast")
        rep = ha.cmd(
            f"broadcast budget preset={cfg['preset']} rtn={cfg['rtn']} phy={cfg['phy']}"
            f" enc={int(cfg.get('enc', 0))}"
        )
        return int(rep["next_big_bis"])
    finally:
        sim.stop()


def label(cfg: dict) -> str:
    return f"{cfg['preset']}-rtn{cfg['rtn']}-{cfg['phy']}{'-enc' if cfg.get('enc') else ''}" + (
        f"-iso{cfg['iso']}" if cfg.get("iso") else ""
    )


def broadcast_cases(quick: bool, presets: list[str] | None = None) -> list[dict]:
    presets = presets or (
        ["16_2", "48_2", "48_6"]
        if quick
        else ["16_2", "24_2", "48_2", "48_4", "48_6", "441_2", "16_1", "48_1", "48_5"]
    )
    rtns = [0, 2] if quick else [0, 2, 4]
    phys = ["2m"] if quick else ["2m", "1m"]
    encs = [0, 1]
    return [
        {"preset": p, "rtn": r, "phy": ph, "enc": e}
        for p, r, ph, e in itertools.product(presets, rtns, phys, encs)
    ]


def one_config(cfg: dict) -> list[dict]:
    n = budget_max(cfg)
    rows = []
    base = {"config": label(cfg), "budget_bis": n}
    if n >= 1:
        r = run_bigs(f"{label(cfg)}-n{n}", [(cfg, n)], force=False)
        rows.append({**base, "bis": n, **r})
    if n < 8:
        r = run_bigs(f"{label(cfg)}-n{n + 1}", [(cfg, n + 1)], force=True)
        rows.append({**base, "bis": n + 1, **r})
    for row in rows:
        clean = row.get("found") == 1 and row.get("lost") == 0
        if row["force"]:
            row["verdict"] = "conservative" if clean else "ok"
        else:
            row["verdict"] = "ok" if clean else "FALSE_FIT"
    return rows


MULTI = [
    # (preset, rtn, BIS count) per BIG
    [("16_2", 0, 8), ("16_2", 0, 3)],
    [("16_2", 0, 3), ("16_2", 0, 3), ("16_2", 0, 2)],
    [("48_2", 2, 2), ("16_2", 2, 2)],
    [("48_4", 0, 2), ("48_4", 0, 2)],
    [("24_2", 1, 3), ("24_2", 1, 2), ("16_2", 0, 2)],
]


def one_multi(spec) -> list[dict]:
    bigs = [
        ({"preset": p, "rtn": r, "phy": "2m", "enc": b > 0}, n) for b, (p, r, n) in enumerate(spec)
    ]
    name = "_".join(f"{p}r{r}x{n}" for p, r, n in spec)
    row = run_bigs(name, bigs, force=False)
    clean = row.get("found") == 1 and row.get("lost") == 0
    row["verdict"] = "ok" if clean or row["model"].startswith("refused") else "FALSE_FIT"
    return [row]


def unicast_case(params) -> list[dict]:
    """A music stream to hearing aid 1 while identities 2..k keep links to hearing aid 2 with the
    given interval (ms): frames the hearing aid misses."""
    others, interval = params
    name = f"unicast-links{others}-int{interval}"
    sim = Sim(OUT / "runs" / name)
    devs = [("ha_tool", "ha_tool"), ("dut", "dut_sim")] + ([("dut2", "dut_sim")] if others else [])
    started = sim.start(devs)
    ha, dut = started[0], started[1]
    row = {"case": name, "links": others + 1, "interval_ms": interval}
    try:
        addr = dut.cmd("addr")["addr"]
        ha.cmd("mode unicast")
        ha.cmd("bonding create A")
        ha.cmd(f"bonding connect A address={addr}")
        ha.wait_evt("bonding_connected", timeout=20)
        if others:
            addr2 = started[2].cmd("addr")["addr"]
            for k in range(others):
                ha.cmd(f"bonding create B{k}")
                ha.cmd(f"bonding connect B{k} address={addr2} interval={interval}")
                ha.wait_evt("bonding_connected", id=k + 2, timeout=20)
        ha.cmd("le-source attach A")
        m = dut.mark()
        ha.cmd("le-source music start preset=48_2 freq=440")
        dut.wait_evt("dut_audio", since=m, src="unicast", timeout=45)
        lost = frames = 0
        for _ in range(MEASURE_S * 2):
            ev = dut.wait_evt("dut_audio", since=dut.mark(), src="unicast", timeout=10)
            lost += int(ev["lost"])
            frames += int(ev["frames"])
        row.update(lost=lost, frames=frames, loss_pct=round(100 * lost / max(frames, 1), 2))
    finally:
        sim.stop()
    return [row]


ACCEPT_PRESETS = ["16_2", "24_2", "32_2", "48_2", "48_4", "48_6", "441_2", "16_1", "48_1", "48_5"]


def accept_matrix(presets: list[str], iso: int = 0) -> list[dict]:
    """One simulation: create and start (--force) every combination, record the controller's
    answer; the reception is not measured."""
    sim = Sim(OUT / "runs" / "accept")
    (ha,) = sim.start([("ha_tool", "ha_tool")])
    rows = []
    try:
        ha.cmd("mode broadcast")
        for preset, rtn, phy, enc in itertools.product(
            presets,
            [0, 1, 2, 4],
            ["2m", "1m"],
            [0, 1],
        ):
            for n in range(1, 9):
                cfg = {"preset": preset, "rtn": rtn, "phy": phy, "enc": enc, "iso": iso}
                show = create(ha, 1, cfg, n)
                mark = ha.mark()
                rep = ha.cmd("broadcast 1 start --force", check=False)
                row = {
                    "config": label(cfg),
                    "bis": n,
                    "airtime_us": show["airtime_us"],
                    "fits": show["fits"],
                    "accepted": int(rep.ok),
                    "errno": rep.errno if not rep.ok else 0,
                }
                if rep.ok:
                    ha.wait_evt("broadcast_started", big=1, timeout=15)
                    layout = [
                        e
                        for e in ha.events[mark:]
                        if e.name == "broadcast_warning" and e.values.get("reason") == "layout"
                    ]
                    row["layout"] = layout[0].raw if layout else ""
                    ha.cmd("broadcast 1 stop")
                ha.cmd("broadcast 1 delete")
                rows.append(row)
                print(row, flush=True)
                if not rep.ok:
                    break  # more BISes only take longer
    finally:
        sim.stop()
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("what", choices=["broadcast", "multi", "unicast", "accept"])
    parser.add_argument("--jobs", type=int, default=4, help="simulations in parallel")
    parser.add_argument("--quick", action="store_true", help="a smaller broadcast matrix")
    parser.add_argument("--presets", default=",".join(ACCEPT_PRESETS), help="accept: presets")
    parser.add_argument("--iso", type=int, default=0, help="accept: common ISO interval (ms)")
    args = parser.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    if args.what == "accept":
        rows = accept_matrix(args.presets.split(","), args.iso)
        keys = list(rows[0])
        with open(OUT / "accept.csv", "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=keys)
            w.writeheader()
            w.writerows(rows)
        keys = list(dict.fromkeys(k for r in rows for k in r))
        with open(OUT / "accept.csv", "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=keys)
            w.writeheader()
            w.writerows(rows)
        ctlr = [r for r in rows if r["errno"] == -22]
        layout = [r for r in rows if r.get("layout")]
        print(
            f"{len(rows)} rows; refused by the controller itself: {len(ctlr)}, "
            f"laid out differently: {len(layout)}"
        )
        return

    if args.what == "broadcast":
        chosen = args.presets.split(",") if args.presets != ",".join(ACCEPT_PRESETS) else None
        cases, fn = broadcast_cases(args.quick, chosen), one_config
    elif args.what == "multi":
        cases, fn = MULTI, one_multi
    else:
        cases = [(0, 0)] + [(k, i) for k in (1, 2) for i in (30, 50, 60, 100)]
        fn = unicast_case

    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(fn, cases):
            for row in result:
                print(row, flush=True)
                rows.append(row)

    keys = list(dict.fromkeys(k for r in rows for k in r))
    path = OUT / f"{args.what}.csv"
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)
    bad = [r for r in rows if r.get("verdict") == "FALSE_FIT"]
    print(f"{len(rows)} rows -> {path}; FALSE_FIT: {len(bad)}")


if __name__ == "__main__":
    main()
