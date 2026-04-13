#!/usr/bin/env python3
"""
Trend / smoothing visualizer for the BaMe firmware.

Runs `_firmware.FirmwareMirror` against a `_battery.Battery` driven by a load
profile from `_scenarios`. Prints one row every output interval with the
trend, gate, EWMA cAvg, and rest state — handy for sanity-checking changes
to the detection logic on a realistic LFP rebound.

Mini-changelog (bugs the sim helped pin down — see CHANGELOG.md for the fixes):
  - v1.13: 100 mV glaciere swings invisible. hist 16→8, slope th 10→5 mV/step.
  - v1.14: maxSliceI gate replaces the instant |current| check (later both).
  - v1.15: SOC blend leaked into the current buffer → 500-1000 W phantom W.
           Fixed by sourcing chist[] from coulombRaw (decoupled integrator).
  - v1.15+: with stable-rest no longer flickering, auto-zero corrupted
            currentOffset in the 10 s lag → readCurrent collapsed to ~0.
            Fixed with an instant |current| < threshold check on the gate.
  - v1.17: 2-point sliding cAvg replaced by EWMA τ=30s (capacitor analog).

Usage:
    python sim/trend_sim.py [--scenario glaciere] [--cycle-on 60] [--cycle-off 120]
                            [--peak 5] [--initial-soc 75] [--duration 1500]
"""

import argparse
import random
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _battery import Battery
from _firmware import FirmwareMirror, TICK_S
import _scenarios as scen

OUTPUT_EVERY_S = 10.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scenario', choices=['glaciere', 'steady', 'mixed', 'charge'],
                    default='glaciere')
    ap.add_argument('--duration', type=int, default=1500,
                    help='seconds to simulate (default 1500)')
    ap.add_argument('--peak', type=float, default=5.0,
                    help='glaciere/steady peak current A')
    ap.add_argument('--cycle-on', type=int, default=60,
                    help='glaciere ON duration s')
    ap.add_argument('--cycle-off', type=int, default=120,
                    help='glaciere OFF duration s')
    ap.add_argument('--initial-soc', type=float, default=75.0)
    ap.add_argument('--true-capacity', type=float, default=80.0)
    ap.add_argument('--nominal-capacity', type=float, default=80.0)
    ap.add_argument('--cells', type=int, default=4)
    ap.add_argument('--noise-v', type=float, default=0.005)
    ap.add_argument('--noise-i', type=float, default=0.005)
    ap.add_argument('--prod', action='store_true',
                    help='use the prod gate (no flat chrono, no preempt)')
    ap.add_argument('--seed', type=int, default=42)
    args = ap.parse_args()

    random.seed(args.seed)

    battery = Battery(true_capacity_ah=args.true_capacity, cells=args.cells,
                      initial_soc=args.initial_soc,
                      voltage_noise=args.noise_v, current_noise=args.noise_i)
    fw = FirmwareMirror(nominal_capacity_ah=args.nominal_capacity,
                        cells=args.cells, dev=not args.prod)

    if args.scenario == 'glaciere':
        gen = scen.glaciere_cycle(idle_s=60, peak=args.peak,
                                  on_s=args.cycle_on, off_s=args.cycle_off,
                                  total_s=args.duration, dt_s=TICK_S)
    elif args.scenario == 'steady':
        gen = scen.steady(current=args.peak, duration_s=args.duration, dt_s=TICK_S)
    elif args.scenario == 'charge':
        gen = scen.charge_session(idle_s=30, charge_a=args.peak,
                                   duration_s=args.duration, dt_s=TICK_S)
    else:
        gen = scen.mixed_van(dt_s=TICK_S)

    print(f"# scenario={args.scenario}  true_cap={args.true_capacity}Ah  "
          f"nominal={args.nominal_capacity}Ah  initial_soc={args.initial_soc}%  "
          f"build={'PROD' if args.prod else 'DEV'}")
    print(f"{'t(s)':>6} {'V':>6} {'arrow':>5} {'cAvg':>6} {'maxSlc':>7} "
          f"{'ina':>5} {'shown':>6} {'chg':>4} {'gate':>6}")
    print("-" * 70)

    last_out = -OUTPUT_EVERY_S
    prev_trend = 0
    prev_stable = False
    prev_chg = False
    for t, true_i in gen:
        battery.step(true_i, TICK_S)
        v_raw = battery.read_voltage(true_i)
        i_raw = battery.read_current(true_i)
        fw.update(v_raw, i_raw, t)

        if t - last_out >= OUTPUT_EVERY_S - 1e-6:
            arrow = {1: 'UP', -1: 'DOWN', 0: ''}[fw.voltage_trend]
            stable_now = fw.dev and (t - fw.flat_since >= 60.0) and (
                fw.buf_min > 0 and fw.max_slice_i < 0.3
                and abs(fw.current) < 0.3 and fw.voltage_trend == 0)
            gate = 'STABLE' if stable_now else ''
            events = []
            if fw.voltage_trend != prev_trend:
                events.append(f"trend->{arrow or 'flat'}")
            if stable_now != prev_stable:
                events.append('ENTER STABLE' if stable_now else 'EXIT STABLE')
            chg_now = fw.external_charge_detected
            if chg_now != prev_chg:
                events.append('CHARGE ON' if chg_now else 'CHARGE OFF')
            marker = (' <<< ' + ', '.join(events)) if events else ''
            chg_str = 'CHG' if chg_now else ''
            print(f"{int(t):6d} {v_raw:6.2f} {arrow:>5} {fw.cAvg:+6.2f} "
                  f"{fw.max_slice_i:>7.2f} {i_raw:+5.2f} {fw.current:+6.2f} "
                  f"{chg_str:>4} {gate:>6}{marker}")
            prev_trend = fw.voltage_trend
            prev_stable = stable_now
            prev_chg = chg_now
            last_out = t


if __name__ == '__main__':
    main()
