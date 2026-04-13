#!/usr/bin/env python3
"""
Voltage + current trend detector simulator.

Ports the firmware rest-detection + smoothed-current logic so we can feed it
realistic profiles and watch the state evolve. The model is the CURRENT,
corrected firmware — bug fixes that used to be CLI toggles are now baked in.

Runs at 100 ms ticks (like the firmware MEASURE_INTERVAL_MS) with a buffer
push every 10 s and a summary row every 10 s. A cyclic glaciere profile is
supported so you can watch what happens when a fresh load starts while
stableRest is active.

Usage:
    python sim/trend_sim.py [--duration 900] [--iload 5]
                            [--idle-s 60] [--drop-s 10] [--load-s 180]
                            [--recov-tau 120] [--vbase 13.30] [--vload 13.10]
                            [--cycle-s 30] [--cycle-idle-s 180]

Mini-changelog of firmware bugs the sim helped pin down and validate:
  - v1.13: 100 mV glaciere swings went undetected. Buffer 16 → 8 samples and
    slope threshold 10 → 5 mV/step fixed it. Sim showed slope never crossed
    the old threshold on a realistic recovery curve.
  - v1.14: sample-based current gate replaces the instant |current| check;
    maxSliceI across the 80 s buffer catches cyclic loads.
  - v1.15: SOC blend rewrites coulombCount while stableRest is active, which
    poisoned the current buffer and produced 500-1000 W phantom readings at
    rest. Fixed by sourcing chist[] from coulombRaw (a parallel integrator
    not touched by the blend). Sim reproduced the spike and validated the
    fix (was --use-raw, now always on).
  - v1.15+: with the cleaner coulombRaw fix, stableRest held steady long
    enough for the auto-zero to corrupt currentOffset during the 10 s lag
    between a fresh load and the next buffer push (A reading of 5 A displayed
    as 0.2 A). Fixed by adding an instant |current| < threshold check on the
    rest gate (was --instant-gate, now always on).
"""

import argparse
import math

# --- Firmware-mirror constants (override via CLI to experiment) ---
DEF_VHIST_SIZE = 8
VHIST_INTERVAL_S = 10          # seconds between samples (VHIST_INTERVAL in firmware)
FLAT_STABLE_S = 60             # countdown target (FLAT_COUNTDOWN_MS / 1000)
DEF_SLOPE_THRESHOLD = 0.005    # V/step — trend ±1 when |slope| > this
FLAT_RETRO_SPREAD = 0.020      # V — retro-range threshold for back-dating chrono
TICK_S = 0.1                   # 100 ms tick (firmware MEASURE_INTERVAL_MS)
OUTPUT_EVERY_S = 10            # table row every N seconds to stay readable


def lr_constants(N):
    """Return (Sx, D) for least-squares slope with x = 0..N-1."""
    Sx  = N * (N - 1) // 2
    Sxx = N * (N - 1) * (2 * N - 1) // 6
    D   = N * Sxx - Sx * Sx
    return Sx, D


def slope_of(buf, Sx, D):
    N = len(buf)
    sumY  = sum(buf)
    sumIY = sum(i * v for i, v in enumerate(buf))
    return (N * sumIY - Sx * sumY) / D


def soc_from_voltage(v, v_empty=12.0, v_full=13.6):
    """Toy linear SOC(%); enough to show blend perturbation."""
    if v <= v_empty: return 0.0
    if v >= v_full:  return 100.0
    return (v - v_empty) / (v_full - v_empty) * 100.0


def simulate(profile, duration_s, hist_size, slope_th,
             retro_spread=FLAT_RETRO_SPREAD,
             current_profile=None, rest_threshold=0.3,
             capacity_ah=80.0, initial_soc=85.0, blend_bias=0.0):
    """Simulate the corrected firmware and yield per-sample state.

    Models the SOC blend and currentOffset auto-zero, both with the fixes
    applied: chist[] is sourced from coulombRaw (not coulombCount) and the
    rest gate requires |current| < rest_threshold in addition to the buffer
    check maxSliceI.

    Yields: (t, v_read, slope, trend, cd, stable, retro_k, cAvg, maxSliceI,
             ina, current_displayed)
    """
    Sx, D = lr_constants(hist_size)
    capacity_as = capacity_ah * 3600.0
    soc_percent = initial_soc
    coulomb_count = (soc_percent / 100.0) * capacity_as
    coulomb_raw = coulomb_count
    current_offset = 0.0
    vhist = [0.0] * hist_size
    chist = [0.0] * hist_size
    vhist_idx = 0
    vhist_count = 0
    last_push_s = -1.0
    flat_since = 0.0
    # Latched (slowly updated) — only refreshed at sample push
    slope = None
    trend = 0
    c_avg = None
    max_slice_i = None
    retro_k = None
    t = 0.0
    last_output_s = -OUTPUT_EVERY_S
    while t <= duration_s:
        ina = current_profile(t) if current_profile is not None else 0.0
        current_corrected = ina - current_offset
        if abs(current_corrected) < 0.05:
            current_corrected = 0.0
        coulomb_count -= current_corrected * TICK_S
        coulomb_raw   -= current_corrected * TICK_S
        if capacity_as > 0:
            soc_percent = max(0.0, min(100.0, coulomb_count / capacity_as * 100.0))

        v_now = profile(t)

        # Sample push every VHIST_INTERVAL_S
        if last_push_s < 0 or (t - last_push_s) >= VHIST_INTERVAL_S:
            vhist[vhist_idx] = v_now
            chist[vhist_idx] = coulomb_raw  # fix: raw integrator, not coulombCount
            vhist_idx = (vhist_idx + 1) % hist_size
            if vhist_count < hist_size:
                vhist_count += 1
            last_push_s = t

            # Analyze buffer on each new sample
            if vhist_count == hist_size:
                oldest_idx = vhist_idx  # newly-rotated = oldest
                vbuf_ordered = [vhist[(oldest_idx + i) % hist_size] for i in range(hist_size)]
                cbuf_ordered = [chist[(oldest_idx + i) % hist_size] for i in range(hist_size)]
                slope = slope_of(vbuf_ordered, Sx, D)
                trend = 1 if slope > slope_th else -1 if slope < -slope_th else 0
                max_slice_i = 0.0
                for i in range(1, hist_size):
                    slice_i = abs(cbuf_ordered[i] - cbuf_ordered[i - 1]) / VHIST_INTERVAL_S
                    if slice_i > max_slice_i:
                        max_slice_i = slice_i

        # 2-point cAvg: refreshed at every tick (LIVE vs oldest snapshot)
        if vhist_count == hist_size:
            oldest_idx = vhist_idx
            age_s = (t - last_push_s) + (hist_size - 1) * VHIST_INTERVAL_S
            c_avg = -(coulomb_raw - chist[oldest_idx]) / age_s

        # Retro-range chrono
        if trend != 0 or vhist_count < hist_size:
            flat_since = t
            retro_k = None
        elif last_push_s == t and vhist_count == hist_size:
            # Recompute retro on sample push
            oldest_idx = vhist_idx
            vbuf_ordered = [vhist[(oldest_idx + i) % hist_size] for i in range(hist_size)]
            newest = vbuf_ordered[-1]
            tMin = tMax = newest
            flat_count = 1
            for back in range(1, hist_size):
                v = vbuf_ordered[-1 - back]
                if v < tMin: tMin = v
                if v > tMax: tMax = v
                if tMax - tMin > retro_spread:
                    break
                flat_count += 1
            retro_k = flat_count
            backdated = t - (flat_count - 1) * VHIST_INTERVAL_S
            if backdated < flat_since:
                flat_since = backdated

        cd = None
        stable = False
        buffer_clean = (max_slice_i is not None
                        and max_slice_i < rest_threshold)
        # fix: instant |current| gate catches a fresh load inside the 10s
        # lag between the last buffer push and maxSliceI picking it up.
        instant_clean = abs(current_corrected) < rest_threshold
        if (trend == 0 and vhist_count == hist_size
                and buffer_clean and instant_clean):
            elapsed = t - flat_since
            if elapsed < FLAT_STABLE_S:
                cd = int(FLAT_STABLE_S - elapsed)
            else:
                stable = True

        if stable:
            soc_v = soc_from_voltage(v_now) + blend_bias
            soc_percent = soc_percent * 0.92 + soc_v * 0.08
            coulomb_count = (soc_percent / 100.0) * capacity_as
            current_offset = current_offset * 0.9 + ina * 0.1

        # Output sparsely
        if t - last_output_s >= OUTPUT_EVERY_S - 1e-6:
            yield (t, v_now, slope, trend, cd, stable, retro_k, c_avg, max_slice_i,
                   ina, current_corrected)
            last_output_s = t

        t += TICK_S


# --- Synthetic profile: one glaciere cycle ---

def glaciere_profile(idle_s, drop_s, load_s, recov_tau_s, v_base, v_load):
    """idle -> ramp down -> load plateau -> log recovery."""
    def f(t):
        if t < idle_s:
            return v_base
        if t < idle_s + drop_s:
            k = (t - idle_s) / drop_s
            return v_base + (v_load - v_base) * k
        if t < idle_s + drop_s + load_s:
            return v_load
        rt = t - (idle_s + drop_s + load_s)
        return v_load + (v_base - v_load) * (1 - math.exp(-rt / recov_tau_s))
    return f


def glaciere_current_profile(idle_s, drop_s, load_s, i_load,
                              cycle_s=0, cycle_idle_s=0):
    """Single or cyclic pull. If cycle_s > 0, after initial cycle, pulls I for
    cycle_s then rests cycle_idle_s, repeating."""
    def f(t):
        if t < idle_s:
            return 0.0
        if t < idle_s + drop_s:
            k = (t - idle_s) / drop_s
            return i_load * k
        if t < idle_s + drop_s + load_s:
            return i_load
        if cycle_s <= 0:
            return 0.0
        # Cyclic after initial load ends
        phase = (t - (idle_s + drop_s + load_s)) % (cycle_s + cycle_idle_s)
        return i_load if phase < cycle_s else 0.0
    return f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--duration', type=int, default=900,
                    help='total seconds to simulate (default 900)')
    ap.add_argument('--idle-s', type=int, default=60,
                    help='seconds of flat idle before load starts')
    ap.add_argument('--drop-s', type=int, default=10,
                    help='seconds for voltage to drop from vbase to vload')
    ap.add_argument('--load-s', type=int, default=180,
                    help='seconds the glaciere keeps pulling')
    ap.add_argument('--recov-tau', type=float, default=120.0,
                    help='log recovery time constant (seconds)')
    ap.add_argument('--vbase', type=float, default=13.30,
                    help='idle / post-recovery voltage')
    ap.add_argument('--vload', type=float, default=13.10,
                    help='voltage under load')
    ap.add_argument('--threshold', type=float, default=DEF_SLOPE_THRESHOLD,
                    help='|slope| threshold in V/step for trend ±1')
    ap.add_argument('--hist', type=int, default=DEF_VHIST_SIZE,
                    help='sample buffer size (default 8)')
    ap.add_argument('--iload', type=float, default=5.0,
                    help='glaciere pull current in A (default 5.0)')
    ap.add_argument('--rest-i', type=float, default=0.3,
                    help='rest-current threshold (VBAT_REST_CURRENT)')
    ap.add_argument('--initial-soc', type=float, default=85.0,
                    help='initial SOC in percent (default 85)')
    ap.add_argument('--blend-bias', type=float, default=-10.0,
                    help='percent bias added to socFromVoltage so the blend '
                         'creates a visible perturbation (default -10)')
    ap.add_argument('--cycle-s', type=int, default=0,
                    help='cyclic glaciere ON duration after initial load (s)')
    ap.add_argument('--cycle-idle-s', type=int, default=0,
                    help='cyclic glaciere OFF duration between pulls (s)')
    args = ap.parse_args()

    prof = glaciere_profile(args.idle_s, args.drop_s, args.load_s,
                            args.recov_tau, args.vbase, args.vload)
    iprof = glaciere_current_profile(args.idle_s, args.drop_s,
                                     args.load_s, args.iload,
                                     args.cycle_s, args.cycle_idle_s)

    print(f"# Parameters: hist={args.hist} samples @ {VHIST_INTERVAL_S}s = "
          f"{args.hist * VHIST_INTERVAL_S}s window, "
          f"slope threshold=+-{args.threshold*1000:.1f} mV/step, "
          f"rest I<{args.rest_i}A")
    print(f"# Profile: idle {args.idle_s}s, drop {args.drop_s}s to {args.vload}V / "
          f"{args.iload}A, load {args.load_s}s, "
          f"recovery tau={args.recov_tau}s back to {args.vbase}V")
    print()
    print(f"{'t(s)':>6} {'V':>7} {'slope':>11} {'arw':>4} {'cd':>4} "
          f"{'retroK':>7} {'cAvg':>6} {'W':>5} {'maxSlc':>6} "
          f"{'ina':>5} {'shown':>6} {'stable':>7}")
    print("-" * 95)
    prev_trend = 0
    prev_stable = False
    for (t, v, slope, trend, cd, stable, retro_k, c_avg, max_slc,
         ina, shown) in simulate(
            prof, args.duration, args.hist, args.threshold,
            current_profile=iprof, rest_threshold=args.rest_i,
            initial_soc=args.initial_soc, blend_bias=args.blend_bias):
        slope_s = f"{slope*1000:+6.2f}mV" if slope is not None else "   --"
        arrow = {1: 'UP', -1: 'DOWN', 0: ''}[trend]
        cd_s = f"{cd:2d}" if cd is not None else ""
        retro_s = f"{retro_k}" if retro_k is not None else ""
        c_s = f"{c_avg:+.2f}" if c_avg is not None else ""
        w_s = f"{abs(c_avg * v):.0f}" if c_avg is not None else ""
        ms_s = f"{max_slc:.2f}" if max_slc is not None else ""
        ina_s = f"{ina:+.2f}"
        shown_s = f"{shown:+.2f}"
        stable_s = 'STABLE' if stable else ''
        events = []
        if trend != prev_trend and slope is not None:
            events.append(f"trend->{arrow or 'flat'}")
        if stable != prev_stable:
            events.append('ENTER STABLE' if stable else 'EXIT STABLE')
        marker = (' <<< ' + ', '.join(events)) if events else ''
        print(f"{int(t):6d} {v:7.3f} {slope_s:>11} {arrow:>4} {cd_s:>4} "
              f"{retro_s:>7} {c_s:>6} {w_s:>5} {ms_s:>6} "
              f"{ina_s:>5} {shown_s:>6} {stable_s:>7}{marker}")
        prev_trend = trend
        prev_stable = stable


if __name__ == '__main__':
    main()
