#!/usr/bin/env python3
"""
Prototype: bidirectional (aging-aware) capacity learning for BUS installs.

Pure-Python mockup — does NOT touch the C core. Explores replacing the
current "amplitude-max, raise-only" learning (which can only ever raise the
estimate, so it stays optimistic as a pack ages) with a two-anchor measurement
that can move up AND down:

  - Two rest-OCV anchors on the steep parts of the LFP curve:
      FULL  — rest, V/cell >= ~3.40  → SOC ~100 %
      KNEE  — rest, V/cell in the bottom steep region (~2.6–3.15) → SOC read
              from the OCV curve (~5–17 %)
  - On the DISCHARGE leg (FULL -> KNEE), BUS coulomb counting gives the exact
    Ah delivered. Capacity = ΔAh / ΔSOC → an absolute measurement, EWMA-
    smoothed, that shrinks when the pack ages. (The charge leg is not counted —
    it would overestimate capacity, coulombic efficiency < 1.)
  - Cycles that never rest at the knee (soft usage) produce no measurement,
    so the estimate is never wrongly lowered.

We drive both learners against a `_battery.Battery` whose true capacity decays
over ~200 cycles, mixing deep cycles (rest at the knee → a real measurement)
and shallow ones (no knee → nothing learned), and watch which estimate tracks
the truth down.

    python sim/aging_proto.py [--cycles 200] [--seed 42]
"""

import argparse
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _battery import Battery, soc_from_voltage_percell

# --- Shared detector constants ---
CELLS          = 4
DT_S           = 1.0
DISCHARGE_A    = 8.0
CHARGE_A       = 8.0
I_REST         = 0.3      # |I| below this = at rest
DEADBAND_A     = 0.05     # matches firmware ±50 mA dead band
ANCHOR_REST_S  = 60.0     # sustained rest in a region before it counts as an anchor

TOP_V_CELL     = 3.375    # rest V/cell above this = FULL anchor region
KNEE_HI_CELL   = 3.15     # rest V/cell in [KNEE_LO, KNEE_HI] = KNEE anchor region
KNEE_LO_CELL   = 2.60
MIN_DSOC       = 0.30     # require >=30 % SOC swing between anchors to trust a measurement
EWMA_ALPHA     = 0.50     # per-measurement capacity smoothing


def _deadband(i):
    return 0.0 if abs(i) < DEADBAND_A else i


class TwoAnchorLearner:
    """Bidirectional: capacity = EWMA of ΔAh/ΔSOC between FULL and KNEE anchors."""

    def __init__(self, capacity_ah):
        self.cap = capacity_ah
        self.q = 0.0                 # free-running charge integral (A·s), never reset
        self.coulomb = capacity_ah * 3600.0
        self.last_kind = None        # 'full' | 'knee'
        self.last_soc = None
        self.last_q = None
        self.top_s = 0.0
        self.knee_s = 0.0
        self.full_armed = True
        self.knee_armed = True
        self.measurements = 0

    def _anchor(self, kind, v_cell):
        soc = soc_from_voltage_percell(v_cell)          # SOC from rest OCV
        # Measure capacity on the DISCHARGE leg only (FULL -> KNEE): the Ah
        # actually delivered. The charge leg would overestimate it because not
        # all charge is retained (coulombic efficiency < 1). The FULL anchor
        # only re-borders the top / re-syncs SOC.
        if kind == 'knee' and self.last_kind == 'full':
            dsoc = abs(soc - self.last_soc) / 100.0
            if dsoc > MIN_DSOC:
                span_ah = abs(self.q - self.last_q) / 3600.0
                measured = span_ah / dsoc               # extrapolate to full 0–100 %
                self.cap = (1 - EWMA_ALPHA) * self.cap + EWMA_ALPHA * measured
                self.measurements += 1
        self.last_kind, self.last_soc, self.last_q = kind, soc, self.q
        if kind == 'full':
            self.coulomb = self.cap * 3600.0            # re-anchor SOC display

    def step(self, v, i, dt):
        i = _deadband(i)
        self.q += i * dt
        self.coulomb -= i * dt
        vc = v / CELLS
        rest = abs(i) < I_REST

        if rest and vc >= TOP_V_CELL:
            self.top_s += dt
            if self.top_s >= ANCHOR_REST_S and self.full_armed:
                self._anchor('full', vc)
                self.full_armed = False
        else:
            self.top_s = 0.0
            if vc < TOP_V_CELL - 0.02:
                self.full_armed = True

        if rest and KNEE_LO_CELL <= vc <= KNEE_HI_CELL:
            self.knee_s += dt
            if self.knee_s >= ANCHOR_REST_S and self.knee_armed:
                self._anchor('knee', vc)
                self.knee_armed = False
        else:
            self.knee_s = 0.0
            if vc > KNEE_HI_CELL + 0.02:
                self.knee_armed = True


class RaiseOnlyLearner:
    """Mirrors the current firmware: raise capacity to the deepest discharge
    seen since the last FULL. Never lowers → stays optimistic as a pack ages."""

    def __init__(self, capacity_ah):
        self.cap = capacity_ah
        self.coulomb = capacity_ah * 3600.0
        self.coulombs_at_last_full = self.coulomb
        self.max_depth = 0.0
        self.top_s = 0.0
        self.full_armed = True

    def step(self, v, i, dt):
        i = _deadband(i)
        self.coulomb -= i * dt
        depth = self.coulombs_at_last_full - self.coulomb
        if depth > self.max_depth:
            self.max_depth = depth
        vc = v / CELLS
        rest = abs(i) < I_REST

        if rest and vc >= TOP_V_CELL:
            self.top_s += dt
            if self.top_s >= ANCHOR_REST_S and self.full_armed:
                observed = self.max_depth / 3600.0
                if observed > self.cap:
                    self.cap = observed
                self.coulomb = self.cap * 3600.0
                self.coulombs_at_last_full = self.coulomb
                self.max_depth = 0.0
                self.full_armed = False
        else:
            self.top_s = 0.0
            if vc < TOP_V_CELL - 0.02:
                self.full_armed = True


class CoreAdapter:
    """Drives the REAL compiled C core (src/bame_core.c via ctypes) through the
    same .step(v, i, dt) interface, so the van scenario validates the firmware
    implementation itself, not just the Python reimplementation."""

    def __init__(self, capacity_ah):
        from bame_core import BameCore
        self.core = BameCore(cells=CELLS, wiring_bus=True, capacity_ah=capacity_ah)
        self.core.declare_full(0)
        self.now_ms = 0
        self.measurements = 0   # not surfaced by the C core; left at 0

    def step(self, v, i, dt):
        self.now_ms += int(dt * 1000)
        self.core.step(v, i, dt, self.now_ms)

    @property
    def cap(self):
        return self.core.capacity_ah


def _feed(learners, v, i, dt):
    for lr in learners:
        lr.step(v, i, dt)


def run_cycle(battery, learners, deep):
    """One discharge → rest → recharge → top-rest cycle. Deep cycles rest at
    the knee (a bidirectional measurement); shallow ones stop mid-plateau."""
    target_soc = 10.0 if deep else 50.0

    # Discharge
    while battery.true_soc > target_soc:
        battery.step(DISCHARGE_A, DT_S)
        _feed(learners, battery.read_voltage(DISCHARGE_A),
              battery.read_current(DISCHARGE_A), DT_S)

    # Rest (long enough for OCV to settle → clean anchor). Deep cycle: at knee.
    for _ in range(int(120 / DT_S)):
        battery.step(0.0, DT_S)
        _feed(learners, battery.read_voltage(0.0), battery.read_current(0.0), DT_S)

    # Recharge (BUS: charge current is visible → negative I)
    while battery.true_soc < 99.5:
        battery.step(-CHARGE_A, DT_S)
        _feed(learners, battery.read_voltage(-CHARGE_A),
              battery.read_current(-CHARGE_A), DT_S)

    # Top rest → FULL anchor
    for _ in range(int(120 / DT_S)):
        battery.step(0.0, DT_S)
        _feed(learners, battery.read_voltage(0.0), battery.read_current(0.0), DT_S)


def _charge_to(battery, learners, target_soc, amps, max_ticks=200000):
    n = 0
    while battery.true_soc < target_soc and n < max_ticks:
        battery.step(-amps, DT_S)
        _feed(learners, battery.read_voltage(-amps), battery.read_current(-amps), DT_S)
        n += 1


def _discharge_to(battery, learners, target_soc, amps, max_ticks=200000):
    n = 0
    while battery.true_soc > target_soc and n < max_ticks:
        battery.step(amps, DT_S)
        _feed(learners, battery.read_voltage(amps), battery.read_current(amps), DT_S)
        n += 1


def _park(battery, learners, seconds):
    for _ in range(int(seconds / DT_S)):
        battery.step(0.0, DT_S)
        _feed(learners, battery.read_voltage(0.0), battery.read_current(0.0), DT_S)


def run_van(battery, learners, macro_cycles, cap_start, cap_end,
            sessions_per_macro=(20, 40), deep_prob=0.30, seed_rng=random):
    """Realistic van usage: each 'vacation' = full charge (departure) → a few
    dozen partial trip/camp sessions → full charge (return). Occasionally a
    long stay deep-discharges to the knee. Pack ages across the whole run.
    Returns per-macro (true_cap, two.cap, ro.cap, anchors_so_far)."""
    two = learners[0]
    battery.true_capacity_as = cap_start * 3600.0
    battery.coulombs_remaining = battery.true_capacity_as
    out = []
    total_sessions = macro_cycles * ((sessions_per_macro[0] + sessions_per_macro[1]) // 2)
    done = 0
    for m in range(macro_cycles):
        # Departure: full charge → FULL anchor + SOC resync.
        _charge_to(battery, learners, 99.5, CHARGE_A)
        _park(battery, learners, 200)

        k = seed_rng.randint(*sessions_per_macro)
        did_deep = False
        for s in range(k):
            # age the pack a touch each session
            frac = min(1.0, done / max(1, total_sessions - 1))
            true_cap = cap_start + (cap_end - cap_start) * frac
            battery.true_capacity_as = true_cap * 3600.0
            if battery.coulombs_remaining > battery.true_capacity_as:
                battery.coulombs_remaining = battery.true_capacity_as
            done += 1

            # One long stay per vacation may deep-discharge to the knee.
            if not did_deep and seed_rng.random() < deep_prob and s > k // 3:
                _discharge_to(battery, learners, 9.0, seed_rng.uniform(1.5, 3.0))
                _park(battery, learners, 200)   # rest at knee → KNEE anchor
                did_deep = True
            else:
                # Camp: partial discharge. Trip: partial charge (20–60 min).
                cur = battery.true_soc
                _discharge_to(battery, learners, max(15.0, cur - seed_rng.uniform(10, 30)),
                              seed_rng.uniform(1.5, 4.0))
                _park(battery, learners, 120)
                cur = battery.true_soc
                _charge_to(battery, learners, min(96.0, cur + seed_rng.uniform(12, 30)),
                           CHARGE_A)
                _park(battery, learners, 120)

        # Return: full charge → FULL anchor.
        _charge_to(battery, learners, 99.5, CHARGE_A)
        _park(battery, learners, 200)

        out.append((true_cap, [lr.cap for lr in learners], learners[0].measurements))
    return out


def sparkline(values, lo, hi, width=None):
    bars = "▁▂▃▄▅▆▇█"
    out = []
    for v in values:
        f = (v - lo) / (hi - lo) if hi > lo else 0.0
        f = max(0.0, min(1.0, f))
        out.append(bars[int(f * (len(bars) - 1))])
    return ''.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cycles', type=int, default=200)
    ap.add_argument('--cap-start', type=float, default=2.0)
    ap.add_argument('--cap-end', type=float, default=1.4)
    ap.add_argument('--nominal', type=float, default=1.7,
                    help='starting estimate (sticker) — below true to show it rise then track down')
    ap.add_argument('--deep-frac', type=float, default=0.6,
                    help='fraction of cycles that rest at the knee')
    ap.add_argument('--usage', choices=['cycles', 'van'], default='cycles',
                    help="'cycles' = clean full cycles; 'van' = partial trip charges "
                         "with a full charge every few dozen sessions")
    ap.add_argument('--macro', type=int, default=12,
                    help="van mode: number of vacation macro-cycles")
    ap.add_argument('--core', action='store_true',
                    help="van mode: also drive the real compiled C core for validation")
    ap.add_argument('--seed', type=int, default=42)
    args = ap.parse_args()

    random.seed(args.seed)

    battery = Battery(true_capacity_ah=args.cap_start, cells=CELLS,
                      initial_soc=100.0, voltage_noise=0.005, current_noise=0.005)
    two  = TwoAnchorLearner(args.nominal)
    ro   = RaiseOnlyLearner(args.nominal)

    if args.usage == 'van':
        learners = [two, ro]
        if args.core:
            learners.append(CoreAdapter(args.nominal))
        print("=== Aging-aware capacity prototype — VAN usage (BUS) ===")
        print(f"true {args.cap_start}→{args.cap_end} Ah, nominal {args.nominal} Ah, "
              f"{args.macro} vacations × 20–40 partial trip/camp sessions, full charge each end")
        rows = run_van(battery, learners, args.macro, args.cap_start, args.cap_end,
                       seed_rng=random)
        hdr = f"{'vac':>4}  {'true':>5}  {'2-anchor':>8}  {'raise-only':>10}"
        if args.core: hdr += f"  {'C-core':>7}"
        print(hdr)
        print("-" * len(hdr))
        for i, (tc, caps, meas) in enumerate(rows, 1):
            line = f"{i:>4}  {tc:5.2f}  {caps[0]:8.2f}  {caps[1]:10.2f}"
            if args.core: line += f"  {caps[2]:7.2f}"
            print(line)
        trues = [r[0] for r in rows]
        twos  = [r[1][0] for r in rows]
        ros   = [r[1][1] for r in rows]
        allv  = trues + twos + ros + ([r[1][2] for r in rows] if args.core else [])
        lo = min(allv) - 0.05; hi = max(allv) + 0.05
        print()
        print(f"true     {sparkline(trues, lo, hi)}  {trues[0]:.2f}→{trues[-1]:.2f}")
        print(f"2-anchor {sparkline(twos, lo, hi)}  {twos[0]:.2f}→{twos[-1]:.2f}  (Python proto)")
        if args.core:
            cores = [r[1][2] for r in rows]
            print(f"C-core   {sparkline(cores, lo, hi)}  {cores[0]:.2f}→{cores[-1]:.2f}  (real bame_core.c)")
        print(f"raise-on {sparkline(ros, lo, hi)}  {ros[0]:.2f}→{ros[-1]:.2f}")
        print()
        msg = (f"final error   2-anchor: {abs(twos[-1]-trues[-1])/trues[-1]*100:5.1f}%   "
               f"raise-only: {abs(ros[-1]-trues[-1])/trues[-1]*100:5.1f}%")
        if args.core:
            msg += f"   C-core: {abs(cores[-1]-trues[-1])/trues[-1]*100:5.1f}%"
        print(msg)
        return

    print("=== Aging-aware capacity prototype (BUS) ===")
    print(f"true capacity {args.cap_start} → {args.cap_end} Ah over {args.cycles} cycles, "
          f"nominal {args.nominal} Ah, {int(args.deep_frac*100)}% deep cycles")
    print(f"{'cyc':>4}  {'true':>5}  {'2-anchor':>8}  {'err':>6}   {'raise-only':>10}  {'err':>6}   kind")
    print("-" * 66)

    trues, twos, ros = [], [], []
    for c in range(1, args.cycles + 1):
        # Linear aging of the true capacity.
        frac = (c - 1) / max(1, args.cycles - 1)
        true_cap = args.cap_start + (args.cap_end - args.cap_start) * frac
        battery.true_capacity_as = true_cap * 3600.0
        battery.coulombs_remaining = battery.true_capacity_as  # start full

        deep = random.random() < args.deep_frac
        run_cycle(battery, [two, ro], deep)

        trues.append(true_cap); twos.append(two.cap); ros.append(ro.cap)
        if c <= 5 or c % 20 == 0 or c == args.cycles:
            print(f"{c:>4}  {true_cap:5.2f}  {two.cap:8.2f}  "
                  f"{(two.cap-true_cap):+6.2f}   {ro.cap:10.2f}  {(ro.cap-true_cap):+6.2f}   "
                  f"{'deep' if deep else 'shallow'}")

    lo = min(min(trues), min(twos), min(ros)) - 0.05
    hi = max(max(trues), max(twos), max(ros)) + 0.05
    print()
    print(f"true     {sparkline(trues, lo, hi)}  {trues[0]:.2f}→{trues[-1]:.2f}")
    print(f"2-anchor {sparkline(twos, lo, hi)}  {twos[0]:.2f}→{twos[-1]:.2f}  ({two.measurements} measurements)")
    print(f"raise-on {sparkline(ros, lo, hi)}  {ros[0]:.2f}→{ros[-1]:.2f}")
    print()
    print(f"final error   2-anchor: {abs(twos[-1]-trues[-1])/trues[-1]*100:5.1f}%   "
          f"raise-only: {abs(ros[-1]-trues[-1])/trues[-1]*100:5.1f}%")


if __name__ == '__main__':
    main()
