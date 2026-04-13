#!/usr/bin/env python3
"""
Capacity-calibration end-to-end simulation — v2 (pure coulomb counting).

Drives the REAL C core (src/bame_core.c, loaded via ctypes) against a
`_battery.Battery` with known true capacity and a load profile from
`_scenarios`. Prints events (full detected, BMS cutoff cycle closed) and
a final summary of how close the learned capacity got to the truth.

Test "battery configured wrong" cases:
  --true-capacity 50 --nominal-capacity 80   # sticker lies
  --cycles 4                                  # repeat → watch convergence

Usage:
    make core-lib            # compile sim/bame_core.dll once
    make sim-cal             # or: python sim/calibration_sim.py ...
"""

import argparse
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _battery import Battery
from bame_core import BameCore, EVT_FULL, EVT_BMS_CUTOFF, EVT_PARTIAL


TICK_S = 0.1
EVENT_NAMES = {0: 'none', 1: 'FULL', 2: 'BMS_CUTOFF', 3: 'PARTIAL'}


def run_one_cycle(core, battery, discharge_a, charge_a, rest_s, bms_cutoff_v,
                   now_ms_ref, verbose=False):
    """Discharge battery until BMS cutoff, rest, recharge. Drives the C core
    with the simulated voltage/current at TICK_S granularity."""
    events = []
    t_local = 0.0
    now_ms = now_ms_ref

    # Discharge phase
    while battery.true_soc > 0.5:
        battery.step(discharge_a, TICK_S)
        v = battery.read_voltage(discharge_a)
        i = battery.read_current(discharge_a)
        evt = core.step(v, i, TICK_S, now_ms)
        if evt != 0:
            events.append((t_local, evt))
            if verbose:
                print(f"   t={t_local:7.0f}s  {EVENT_NAMES[evt]}  "
                      f"core.coulomb={core.coulomb_count:.0f}  cap={core.capacity_ah:.2f}")
        t_local += TICK_S
        now_ms += int(TICK_S * 1000)

    # Force BMS cutoff by dropping voltage briefly — matches real BMS behavior
    for _ in range(10):  # 1 second at cutoff voltage
        evt = core.step(bms_cutoff_v, 0.0, TICK_S, now_ms)
        if evt == EVT_BMS_CUTOFF:
            events.append((t_local, evt))
            if verbose:
                print(f"   t={t_local:7.0f}s  BMS_CUTOFF  "
                      f"delivered={battery.true_capacity_as/3600 - battery.coulombs_remaining/3600:.2f}Ah "
                      f"core.learned={core.capacity_ah:.2f}Ah")
        t_local += TICK_S
        now_ms += int(TICK_S * 1000)

    # Rest
    for _ in range(int(rest_s / TICK_S)):
        evt = core.step(0.0, 0.0, TICK_S, now_ms)
        t_local += TICK_S
        now_ms += int(TICK_S * 1000)

    # Recharge (negative current; in BUS mode this is visible to the core).
    # Wiring LOAD is invisible — the core only sees current=0 during charge.
    battery.coulombs_remaining = 0
    while battery.true_soc < 99.5:
        battery.step(-charge_a, TICK_S)
        v = battery.read_voltage(-charge_a)
        i_core = -charge_a if core.state.wiring_bus else 0.0
        evt = core.step(v, i_core, TICK_S, now_ms)
        if evt != 0:
            events.append((t_local, evt))
            if verbose:
                print(f"   t={t_local:7.0f}s  {EVENT_NAMES[evt]}  "
                      f"core.cap={core.capacity_ah:.2f}")
        t_local += TICK_S
        now_ms += int(TICK_S * 1000)

    # Final rest at top — voltage stays at top OCV, should trigger FULL event
    for _ in range(int(rest_s / TICK_S)):
        battery.step(0.0, TICK_S)
        v = battery.read_voltage(0.0)
        evt = core.step(v, 0.0, TICK_S, now_ms)
        if evt != 0:
            events.append((t_local, evt))
            if verbose:
                print(f"   t={t_local:7.0f}s  {EVENT_NAMES[evt]}  "
                      f"core.coulomb={core.coulomb_count:.0f}")
        t_local += TICK_S
        now_ms += int(TICK_S * 1000)

    return events, now_ms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--true-capacity', type=float, default=50.0,
                    help='real battery capacity in Ah')
    ap.add_argument('--nominal-capacity', type=float, default=80.0,
                    help='what the user typed in the menu')
    ap.add_argument('--cells', type=int, default=4)
    ap.add_argument('--wiring', choices=['bus', 'load'], default='bus')
    ap.add_argument('--cycles', type=int, default=3,
                    help='how many full→cutoff→recharge cycles to run')
    ap.add_argument('--discharge-a', type=float, default=5.0)
    ap.add_argument('--charge-a', type=float, default=5.0)
    ap.add_argument('--rest-s', type=float, default=60.0)
    ap.add_argument('--noise-v', type=float, default=0.005)
    ap.add_argument('--noise-i', type=float, default=0.005)
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    random.seed(args.seed)

    battery = Battery(true_capacity_ah=args.true_capacity, cells=args.cells,
                      initial_soc=100.0,
                      voltage_noise=args.noise_v, current_noise=args.noise_i)
    core = BameCore(cells=args.cells, wiring_bus=(args.wiring == 'bus'),
                    capacity_ah=args.nominal_capacity)

    # Boot: the user declares battery full once at startup (or the auto-
    # detector fires on first at-top + rest). We skip the wait and declare.
    core.declare_full(0)

    print(f"=== BaMe v2 calibration sim ===")
    print(f"true cap {args.true_capacity}Ah  nominal {args.nominal_capacity}Ah  "
          f"{args.cells}S  wiring={args.wiring}  cycles={args.cycles}")
    print(f"boot: user declared full -> core.coulomb={core.coulomb_count:.0f}As "
          f"({core.soc_percent:.0f}%)")
    print()

    bms_cutoff_v = 0.3  # voltage during BMS cutoff event
    now_ms = 0
    cycle_results = []

    for cycle in range(1, args.cycles + 1):
        battery.coulombs_remaining = battery.true_capacity_as * 1.0  # always start full
        if args.verbose:
            print(f"--- Cycle {cycle} ---")
        events, now_ms = run_one_cycle(core, battery,
                                        args.discharge_a, args.charge_a,
                                        args.rest_s, bms_cutoff_v, now_ms,
                                        verbose=args.verbose)
        cycle_results.append((cycle, core.capacity_ah, core.capacity_learned))
        print(f"cycle {cycle}: real={args.true_capacity}Ah  "
              f"learned={core.capacity_ah:.2f}Ah  "
              f"err={abs(core.capacity_ah - args.true_capacity):.2f}Ah  "
              f"({'learned' if core.capacity_learned else 'not yet learned'})")

    print()
    print(f"=== Result ===")
    print(f"true capacity:    {args.true_capacity:.2f} Ah")
    print(f"learned capacity: {core.capacity_ah:.2f} Ah "
          f"(error {abs(core.capacity_ah - args.true_capacity)/args.true_capacity*100:.1f}%)")
    print(f"soc_uncertain:    {core.soc_uncertain}")


if __name__ == '__main__':
    main()
