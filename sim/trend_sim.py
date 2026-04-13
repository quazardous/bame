#!/usr/bin/env python3
"""
Voltage trend detector simulator.

Ports the firmware's 16-sample linear regression slope detection and the
60 s flat chrono so we can feed it realistic voltage profiles and see what
the device would display step by step.

Usage:
    python sim/trend_sim.py [--duration 900] [--vbase 13.30] [--vload 13.10]
                            [--load-s 180] [--recov-tau 120]
                            [--threshold 0.010] [--hist 16]

The default profile is a single glaciere cycle:
    [idle] [rapid drop] [load plateau] [log recovery]
"""

import argparse
import math

# --- Firmware-mirror constants (override via CLI to experiment) ---
DEF_VHIST_SIZE = 8
VHIST_INTERVAL_S = 10          # seconds between samples (VHIST_INTERVAL in firmware)
FLAT_STABLE_S = 60             # countdown target (FLAT_COUNTDOWN_MS / 1000)
DEF_SLOPE_THRESHOLD = 0.005    # V/step — trend ±1 when |slope| > this
FLAT_RETRO_SPREAD = 0.020      # V — retro-range threshold for back-dating chrono


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


def simulate(profile, duration_s, hist_size, slope_th,
             retro_spread=FLAT_RETRO_SPREAD,
             current_profile=None, rest_threshold=0.3):
    """Yield per-sample state.

    profile(t)          -> voltage at time t
    current_profile(t)  -> current at time t (optional, default 0 A)

    Yields: (t, v, slope, trend, cd, stable, retro_k, cAvg, maxSliceI)
    """
    Sx, D = lr_constants(hist_size)
    vbuf = []
    cbuf = []           # coulombCount snapshots (A·s)
    coulomb_count = 0.0 # running integrator (A·s); sign: decreases on discharge
    flat_since = 0
    t = 0
    prev_t = 0
    while t <= duration_s:
        # Integrate current continuously (at the sample rate in this sim).
        if current_profile is not None:
            i_now = current_profile(t)
            dt = t - prev_t
            coulomb_count -= i_now * dt
        prev_t = t

        vbuf.append(profile(t))
        cbuf.append(coulomb_count)
        if len(vbuf) > hist_size:
            vbuf.pop(0)
            cbuf.pop(0)

        c_avg = None
        max_slice_i = None
        if len(vbuf) == hist_size:
            slope = slope_of(vbuf, Sx, D)
            trend = 1 if slope > slope_th else -1 if slope < -slope_th else 0
            # Current: slope on coulomb buffer → average current over window.
            slope_c = slope_of(cbuf, Sx, D)  # A·s per step
            c_avg = -slope_c / VHIST_INTERVAL_S
            # Max current over any 10s slice = max |ΔcoulombCount| / 10s.
            max_slice_i = 0
            for i in range(1, hist_size):
                slice_i = abs(cbuf[i] - cbuf[i - 1]) / VHIST_INTERVAL_S
                if slice_i > max_slice_i:
                    max_slice_i = slice_i
        else:
            slope = None
            trend = 0

        # Path (a): reset chrono while arrow up/down or buffer warming up.
        retro_k = None
        if trend != 0 or len(vbuf) < hist_size:
            flat_since = t
        elif len(vbuf) == hist_size:
            # Path (b): retro-range back-dating when flat detected.
            newest = vbuf[-1]
            tMin = tMax = newest
            flat_count = 1
            for back in range(1, hist_size):
                v = vbuf[-1 - back]
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
        # stableRest now also requires maxSliceI < rest_threshold.
        current_clean = (max_slice_i is not None
                         and max_slice_i < rest_threshold)
        if trend == 0 and len(vbuf) == hist_size and current_clean:
            elapsed = t - flat_since
            if elapsed < FLAT_STABLE_S:
                cd = FLAT_STABLE_S - elapsed
            else:
                stable = True

        yield (t, vbuf[-1], slope, trend, cd, stable, retro_k, c_avg, max_slice_i)
        t += VHIST_INTERVAL_S


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


def glaciere_current_profile(idle_s, drop_s, load_s, i_load):
    """0 A idle, ramp up, load current during pull, 0 A after."""
    def f(t):
        if t < idle_s:
            return 0.0
        if t < idle_s + drop_s:
            k = (t - idle_s) / drop_s
            return i_load * k
        if t < idle_s + drop_s + load_s:
            return i_load
        return 0.0
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
    args = ap.parse_args()

    prof = glaciere_profile(args.idle_s, args.drop_s, args.load_s,
                            args.recov_tau, args.vbase, args.vload)
    iprof = glaciere_current_profile(args.idle_s, args.drop_s,
                                     args.load_s, args.iload)

    print(f"# Parameters: hist={args.hist} samples @ {VHIST_INTERVAL_S}s = "
          f"{args.hist * VHIST_INTERVAL_S}s window, "
          f"slope threshold=+-{args.threshold*1000:.1f} mV/step, "
          f"rest I<{args.rest_i}A")
    print(f"# Profile: idle {args.idle_s}s, drop {args.drop_s}s to {args.vload}V / "
          f"{args.iload}A, load {args.load_s}s, "
          f"recovery tau={args.recov_tau}s back to {args.vbase}V")
    print()
    print(f"{'t(s)':>6} {'V':>7} {'slope':>11} {'arw':>4} {'cd':>4} "
          f"{'retroK':>7} {'cAvg(A)':>8} {'maxSlc':>7} {'stable':>7}")
    print("-" * 75)
    prev_trend = 0
    prev_stable = False
    for t, v, slope, trend, cd, stable, retro_k, c_avg, max_slc in simulate(
            prof, args.duration, args.hist, args.threshold,
            current_profile=iprof, rest_threshold=args.rest_i):
        slope_s = f"{slope*1000:+6.2f}mV" if slope is not None else "   --"
        arrow = {1: 'UP', -1: 'DOWN', 0: ''}[trend]
        cd_s = f"{cd:2d}" if cd is not None else ""
        retro_s = f"{retro_k}" if retro_k is not None else ""
        c_s = f"{c_avg:+.2f}" if c_avg is not None else ""
        ms_s = f"{max_slc:.2f}" if max_slc is not None else ""
        stable_s = 'STABLE' if stable else ''
        events = []
        if trend != prev_trend and slope is not None:
            events.append(f"trend->{arrow or 'flat'}")
        if stable != prev_stable:
            events.append('ENTER STABLE' if stable else 'EXIT STABLE')
        marker = (' <<< ' + ', '.join(events)) if events else ''
        print(f"{t:6d} {v:7.3f} {slope_s:>11} {arrow:>4} {cd_s:>4} "
              f"{retro_s:>7} {c_s:>8} {ms_s:>7} {stable_s:>7}{marker}")
        prev_trend = trend
        prev_stable = stable


if __name__ == '__main__':
    main()
