#!/usr/bin/env python3
"""
Prototype: fix the two long-term-drift bugs in the coulomb integrator.

Pure-Python mockup — does NOT touch the C core. Python floats are 64-bit, so
every arithmetic step is forced back through float32 (`f32()`) to reproduce the
AVR/`bame_core.c` behaviour exactly.

Two independent bugs make BaMe under-count discharge, and they compound:

1. FLOAT32 QUANTISATION. `coulomb_count` holds ~288000 (80 Ah in A·s). At that
   magnitude a float32 ULP is 0.03125, but a 100 ms tick at 0.45 A only
   subtracts 0.045 — just 1.44 ULP — so rounding drags each step back to
   0.03125 and ~31 % of the charge is silently lost. Below ~0.156 A the step
   falls under ½ ULP and the counter stops moving at all. It gets worse on
   bigger packs (bigger accumulator → bigger ULP).
   FIX: integrate into a small fractional accumulator that stays near 0 (full
   precision) and only hand whole A·s over to the big counter.

2. OFFSET AUTO-ZERO EATING REAL LOADS. The auto-zero is meant to track the
   INA226's thermal offset drift (a few mA, very slow), but it triggers on
   |I| < i_rest (0.3 A) with τ ≈ 10 s — so any genuine load under 0.3 A is
   absorbed into `current_offset` within ~30 s and becomes invisible.
   FIX: only auto-zero when the reading is already inside the dead band (i.e.
   we're confident nothing is drawing) and slow τ down to ~1 h, which is what a
   thermal drift actually calls for.

    python sim/integrator_proto.py
"""

import struct


def f32(x):
    """Round a Python float to float32, as the AVR would store it."""
    return struct.unpack('f', struct.pack('f', x))[0]


TICK_S     = 0.1
DEADBAND_A = 0.05
I_REST     = 0.3


class Integrator:
    """`fixed=False` mirrors bame_core.c as shipped; `fixed=True` applies both fixes."""

    def __init__(self, capacity_ah=80.0, fixed=False, offset0=0.0):
        self.fixed   = fixed
        self.coulomb = f32(capacity_ah * 3600.0)
        self.frac    = 0.0          # fixed mode only: small, near-zero accumulator
        self.offset  = f32(offset0)

    def step(self, current_raw, dt=TICK_S):
        c = f32(current_raw - self.offset)
        if abs(c) < DEADBAND_A:
            c = 0.0

        if not self.fixed:
            # --- shipped: subtract straight into the big accumulator ---
            self.coulomb = f32(self.coulomb - f32(c * dt))
            # --- shipped: auto-zero on |I| < 0.3 A, tau ~ 10 s ---
            if abs(c) < I_REST:
                raw = f32(c + self.offset)
                self.offset = f32(f32(self.offset * 0.99) + f32(raw * 0.01))
        else:
            # --- fix 1: accumulate the fraction near zero, transfer whole A·s ---
            self.frac = f32(self.frac - f32(c * dt))
            if abs(self.frac) >= 1.0:
                whole = float(int(self.frac))          # trunc toward zero
                self.coulomb = f32(self.coulomb + whole)
                self.frac    = f32(self.frac - whole)
            # --- fix 2: only auto-zero when dead-banded (no real load), tau ~ 1 h ---
            if c == 0.0:
                a = 0.1 / 3600.0
                self.offset = f32(f32(self.offset * (1.0 - a)) + f32(current_raw * a))

    def counted_ah(self, start_coulomb):
        """Ah counted so far. `frac` is negative while discharging (it holds the
        not-yet-transferred part), so it is SUBTRACTED, not added."""
        pending = -self.frac if self.fixed else 0.0
        return (start_coulomb - self.coulomb + pending) / 3600.0


def run(load_a, hours, fixed, capacity_ah=80.0, offset0=0.0):
    it = Integrator(capacity_ah, fixed, offset0)
    start = it.coulomb
    for _ in range(int(hours * 3600 / TICK_S)):
        it.step(load_a)
    return it.counted_ah(start), it.offset


def run_fridge(hours, fixed, on_a=6.0, on_s=120, off_s=240):
    """Duty-cycled compressor:true mean = on_a * on_s / (on_s + off_s)."""
    it = Integrator(80.0, fixed)
    start = it.coulomb
    period = on_s + off_s
    for i in range(int(hours * 3600 / TICK_S)):
        t = (i * TICK_S) % period
        it.step(on_a if t < on_s else 0.0)
    real = on_a * on_s / period * hours
    return it.counted_ah(start), real


def main():
    print("=== Steady loads, 1 h, 80 Ah pack — counted vs real ===")
    print(f"{'load':>6}  {'real':>7}  {'shipped':>9} {'err':>7}   {'fixed':>8} {'err':>7}")
    print("-" * 56)
    for load in (0.10, 0.20, 0.45, 1.0, 2.0, 5.0, 20.0):
        real = load * 1.0
        cur, _ = run(load, 1.0, fixed=False)
        fix, _ = run(load, 1.0, fixed=True)
        print(f"{load:6.2f}  {real:7.3f}  {cur:9.3f} {(cur-real)/real*100:6.1f}%   "
              f"{fix:8.3f} {(fix-real)/real*100:6.1f}%")

    print()
    print("=== Same, on a 200 Ah pack (bigger accumulator → bigger ULP) ===")
    for load in (0.45, 2.0):
        real = load * 1.0
        cur, _ = run(load, 1.0, fixed=False, capacity_ah=200.0)
        fix, _ = run(load, 1.0, fixed=True, capacity_ah=200.0)
        print(f"{load:6.2f}  {real:7.3f}  {cur:9.3f} {(cur-real)/real*100:6.1f}%   "
              f"{fix:8.3f} {(fix-real)/real*100:6.1f}%")

    print()
    print("=== Fridge (6 A, 2 min on / 4 min off = 2.0 A mean), 24 h ===")
    cur, real = run_fridge(24, fixed=False)
    fix, _    = run_fridge(24, fixed=True)
    print(f"  real {real:6.2f} Ah   shipped {cur:6.2f} Ah ({(cur-real)/real*100:+.1f}%)"
          f"   fixed {fix:6.2f} Ah ({(fix-real)/real*100:+.1f}%)")

    print()
    print("=== Real INA226 offset drift (+4 mA) with NO load, 24 h ===")
    print("    (the auto-zero's actual job: this must stay ~0 Ah)")
    cur, off_c = run(0.004, 24, fixed=False)
    fix, off_f = run(0.004, 24, fixed=True)
    print(f"  shipped {cur:6.3f} Ah  (offset learned {off_c:.4f} A)")
    print(f"  fixed   {fix:6.3f} Ah  (offset learned {off_f:.4f} A)")


if __name__ == '__main__':
    main()
