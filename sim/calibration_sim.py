#!/usr/bin/env python3
"""
Capacity-calibration end-to-end simulation.

Drives `_firmware.FirmwareMirror` against a `_battery.Battery` with a
known true capacity, using a load profile from `_scenarios`. Prints one
line per calibration event (open / preempt / commit / rollback) and a
final summary of how close the estimate got to the truth.

Use it to test "battery configured wrong" cases:
  --true-capacity 50 --nominal-capacity 80
  --true-cell-full 3.55  (real pack tops at 3.55, not 3.65)
  --user-vmin-utile 11.5 (user typed wrong)

Usage:
    python sim/calibration_sim.py [--scenario mixed] [--true-capacity 50]
                                  [--nominal-capacity 80] [--cells 4]
"""

import argparse
import random
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _battery import Battery
from _firmware import FirmwareMirror, TICK_S
import _scenarios as scen


SCENARIOS = ['steady', 'glaciere', 'mixed', 'long', 'deep']


def make_scenario(name, dt_s):
    if name == 'steady':
        return scen.steady(current=5.0, duration_s=4 * 3600, dt_s=dt_s)
    if name == 'glaciere':
        return scen.glaciere_cycle(total_s=8 * 3600, dt_s=dt_s)
    if name == 'mixed':
        return scen.mixed_van(dt_s=dt_s)
    if name == 'long':
        return scen.long_steady(dt_s=dt_s)
    if name == 'deep':
        return scen.deep_cycle(dt_s=dt_s)
    raise SystemExit(f"unknown scenario: {name}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scenario', choices=SCENARIOS, default='mixed')
    ap.add_argument('--true-capacity', type=float, default=50.0,
                    help='real battery capacity in Ah')
    ap.add_argument('--nominal-capacity', type=float, default=80.0,
                    help='what the user typed in the menu')
    ap.add_argument('--cells', type=int, default=4)
    ap.add_argument('--initial-soc', type=float, default=85.0)
    ap.add_argument('--noise-v', type=float, default=0.005)
    ap.add_argument('--noise-i', type=float, default=0.005)
    ap.add_argument('--prod', action='store_true',
                    help='run the prod gate (single-shot save) instead of dev')
    ap.add_argument('--seed', type=int, default=42)
    args = ap.parse_args()

    random.seed(args.seed)

    battery = Battery(true_capacity_ah=args.true_capacity, cells=args.cells,
                      initial_soc=args.initial_soc,
                      voltage_noise=args.noise_v, current_noise=args.noise_i)
    fw = FirmwareMirror(nominal_capacity_ah=args.nominal_capacity,
                        cells=args.cells, dev=not args.prod)
    # Boot sync (mirror of firmware setup)
    fw.init_soc_from_voltage(battery.read_voltage(0))

    print(f"=== BaMe calibration sim ===")
    print(f"true cap {args.true_capacity}Ah  nominal {args.nominal_capacity}Ah  "
          f"{args.cells}S  build={'PROD' if args.prod else 'DEV'}  "
          f"scenario={args.scenario}")
    print(f"initial SOC: true={battery.true_soc:.1f}%  est={fw.soc_percent:.1f}%")
    print()

    # Track gauge accuracy across the run: pairs (t, true_soc, shown_soc)
    gauge_history = []
    next_commit_idx = 0

    for t, true_i in make_scenario(args.scenario, TICK_S):
        battery.step(true_i, TICK_S)
        v_raw = battery.read_voltage(true_i)
        i_raw = battery.read_current(true_i)
        fw.update(v_raw, i_raw, t)

        # Snapshot gauge reading every 30 s for the table
        if not gauge_history or t - gauge_history[-1][0] >= 30.0:
            gauge_history.append((t, battery.true_soc, fw.soc_percent,
                                  battery.coulombs_remaining / 3600.0,
                                  fw.coulomb_count / 3600.0))

    # Final summary
    print(f"\n=== Result ===")
    print(f"true SOC final:  {battery.true_soc:6.2f}%")
    print(f"est SOC final:   {fw.soc_percent:6.2f}%")
    print(f"true capacity:   {args.true_capacity:6.2f} Ah")
    print(f"est  capacity:   {fw.battery_capacity_ah:6.2f} Ah  "
          f"(error {abs(fw.battery_capacity_ah - args.true_capacity)/args.true_capacity*100:.1f}%)")
    print(f"vbat_top final:  {fw.vbat_top:6.3f} V  ({fw.vbat_top/args.cells:.3f} V/cell)")
    print(f"current offset:  {fw.current_offset*1000:+5.1f} mA")

    # Gauge accuracy: SOC% shown vs true at each commit
    if fw.cap_estimates and gauge_history:
        print(f"\nGauge tracking around commit events:")
        print(f"  {'t(s)':>7s} {'capAh':>7s} {'true SOC':>9s} {'shown SOC':>10s} "
              f"{'true Ah':>8s} {'shown Ah':>9s} {'soc err':>8s}")
        for t_commit, est, ds, cap, ok in fw.cap_estimates:
            # Find nearest gauge sample
            sample = min(gauge_history, key=lambda g: abs(g[0] - t_commit))
            tg, ts, ss, tah, sah = sample
            tag = 'OK' if ok else 'rej'
            soc_err = ss - ts
            print(f"  {int(tg):7d} {cap:7.1f} {ts:8.2f}% {ss:9.2f}% "
                  f"{tah:7.2f}A {sah:8.2f}A {soc_err:+7.2f}% {tag}")

    # Final gauge state
    if gauge_history:
        t_end, ts_end, ss_end, tah_end, sah_end = gauge_history[-1]
        print(f"\nFinal gauge state (t={int(t_end)}s):")
        print(f"  true:    SOC {ts_end:5.1f}%   {tah_end:5.1f} Ah / {args.true_capacity:.1f} Ah")
        print(f"  shown:   SOC {ss_end:5.1f}%   {sah_end:5.1f} Ah / {fw.battery_capacity_ah:.1f} Ah")
        print(f"  error:   {ss_end - ts_end:+5.1f}% SOC,  {sah_end - tah_end:+5.1f} Ah")
        # ASCII bar (40 chars wide)
        def bar(pct, label):
            n = int(max(0, min(100, pct)) * 40 / 100)
            return f"  {label} [{'#'*n}{'.'*(40-n)}] {pct:5.1f}%"
        print(bar(ts_end, 'true ') )
        print(bar(ss_end, 'shown'))

    if fw.events:
        print(f"\nEvent log ({len(fw.events)} events):")
        for t, kind, payload in fw.events:
            print(f"  {int(t):7d}s  {kind:<20s}  {payload}")

    if fw.cap_estimates:
        print(f"\nCalibration history ({len(fw.cap_estimates)} commits/rejects):")
        print(f"  {'t(s)':>7s} {'estAh':>7s} {'dSOC':>5s} {'capAh':>7s} {'err':>5s}  status")
        for t, est, ds, cap, ok in fw.cap_estimates:
            est_str = f"{est:7.1f}" if est is not None else "  --   "
            err = abs((est or 0) - args.true_capacity) / args.true_capacity * 100 if est else 0
            err_str = f"{err:4.1f}%"
            tag = 'OK' if ok else 'rej'
            print(f"  {int(t):7d} {est_str} {ds:4.1f}% {cap:7.1f} {err_str}  {tag}")
    else:
        print("(no calibration estimates produced — scenario too short or no rest)")


if __name__ == '__main__':
    main()
