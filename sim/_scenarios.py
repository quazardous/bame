"""Reusable load profiles. Each generator yields (t_seconds, true_current_A).

Discharge is positive (battery losing coulombs), charge is negative.
"""

import math


def steady(current=5.0, duration_s=600, dt_s=0.1, idle_s=15):
    """Idle, then a constant load for the given duration."""
    t = 0.0
    while t < idle_s:
        yield t, 0.0
        t += dt_s
    end = t + duration_s
    while t < end:
        yield t, current
        t += dt_s


def glaciere_cycle(idle_s=60, drop_s=10, peak=5.0, on_s=60, off_s=120,
                   total_s=3600, dt_s=0.1):
    """Idle, then ON/OFF cycling forever (compressor pattern)."""
    t = 0.0
    while t < idle_s:
        yield t, 0.0
        t += dt_s
    cycle = on_s + off_s
    end = t + total_s
    cycle_start = t
    # initial ramp-up
    ramp_end = t + drop_s
    while t < ramp_end:
        yield t, peak * (t - cycle_start) / drop_s
        t += dt_s
    while t < end:
        phase = (t - cycle_start - drop_s) % cycle
        yield t, (peak if phase < on_s else 0.0)
        t += dt_s


def mixed_van(dt_s=0.1, rest_s=600):
    """Realistic van scenario: long fridge run, heavy load, partial charge,
    repeat. Rest periods are long enough (default 10 min) to clear the LFP
    polarization rebound AND let the firmware flat-chrono confirm. With
    rest_s < ~300 the calibration won't get any segment commits."""
    events = []
    def add(current, duration):
        n = int(duration / dt_s)
        for _ in range(n):
            events.append(current)
    add(0.0, 300)               # boot rest (5 min so buffer fills + chrono can fire)
    add(2.0, 1800)              # 30 min fridge
    add(0.0, rest_s)
    add(8.0, 900)               # 15 min heavy
    add(0.0, rest_s)
    add(-3.0, 300)              # 5 min solar charge → invalidates segment
    add(0.0, rest_s)
    add(5.0, 2700)              # 45 min mixed
    add(0.0, rest_s)
    add(1.0, 3600)              # 60 min light
    add(0.0, rest_s)
    add(10.0, 1200)             # 20 min heavy
    add(0.0, rest_s)            # final rest
    for i, c in enumerate(events):
        yield i * dt_s, c


def long_steady(rate_a=3.0, blocks=8, block_s=1800, rest_s=600, dt_s=0.1):
    """Discharge in blocks of 30 min with 10 min rests so the calibration
    actually completes segments. Default: 8 blocks × 30 min × 3 A = 12 Ah
    total — enough for several SOC% drops on a 50 Ah pack."""
    events = []
    def add(current, duration):
        n = int(duration / dt_s)
        for _ in range(n):
            events.append(current)
    add(0.0, 300)               # initial rest (5 min)
    for _ in range(blocks):
        add(rate_a, block_s)
        add(0.0, rest_s)
    add(0.0, 600)               # extra final rest
    for i, c in enumerate(events):
        yield i * dt_s, c


def charge_session(idle_s=60, charge_a=5.0, duration_s=3600, dt_s=0.1):
    """Idle, then a constant charging current (negative). Reproduces a
    charger plug-in: voltage starts wherever the battery is, rises through
    the LFP_CELL_CHARGE_MIN threshold, eventually external charge is
    detected and the icon appears."""
    t = 0.0
    while t < idle_s:
        yield t, 0.0
        t += dt_s
    end = t + duration_s
    while t < end:
        yield t, -charge_a
        t += dt_s


def deep_cycle(rate_a=5.0, segments=3, segment_s=3600, rest_s=900, dt_s=0.1):
    """Bigger cycles to get past the LFP flat curve. Each segment drops the
    SOC by `rate_a × segment_s / 3600` Ah. Default 3 × 1h × 5A = 15 Ah,
    which is 30% on a 50 Ah pack — well into the steep edges of the curve."""
    events = []
    def add(current, duration):
        n = int(duration / dt_s)
        for _ in range(n):
            events.append(current)
    add(0.0, 300)
    for _ in range(segments):
        add(rate_a, segment_s)
        add(0.0, rest_s)
    add(0.0, 600)
    for i, c in enumerate(events):
        yield i * dt_s, c
