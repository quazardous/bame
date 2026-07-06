#!/usr/bin/env python3
"""
Genetic optimizer for the BaMe v2 core thresholds (bame_config_t).

Evolves the detection/learning thresholds by running full → deep-discharge
→ recharge cycles of the REAL C core (src/bame_core.c via ctypes) against
the synthetic LFP battery, minimizing capacity-learning error and final
SOC drift. Because it drives the exact code that runs on the AVR, a
winning genome maps 1:1 onto `bame_config_defaults()`.

Uses small fast packs (2 Ah at 8 A) so a genome evaluates in ~1-2 s at the
firmware tick rate (100 ms). Expect ~5-10 min with the defaults.

Usage:
    make core-lib     # compile sim/bame_core.dll once
    python sim/optimize.py [--generations 15] [--population 20]
"""

import argparse
import copy
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from _battery import Battery
from bame_core import BameCore, EVT_FULL
from calibration_sim import run_one_cycle

# ============================================================
# Parameters to optimize: (name, min, max, firmware default)
# Times are in seconds here; converted to ms when written into the config.
# cavg_ewma_alpha (display smoothing) and cap_min/max (sanity bounds) are
# deliberately not evolved — they don't affect learning or SOC.
# ============================================================
PARAM_DEFS = [
    ('v_full_per_cell',   3.30, 3.55, 3.40),
    ('i_rest',            0.05, 1.00, 0.30),
    ('full_rest_s',       5.0,  60.0, 30.0),
    ('v_rise_partial',    0.01, 0.30, 0.05),
    ('v_disconnect_drop', 0.10, 1.00, 0.50),
    ('ext_rearm_s',       2.0,  60.0, 15.0),
]

# Test conditions: (true_ah, nominal_ah, wiring_bus, cycles).
# nominal < true exercises the amplitude-max raise (needs a few cycles to
# climb past the ±10% integrator clamp); nominal == true checks that a
# correct estimate stays put and SOC tracks.
TEST_CASES = [
    (2.0, 1.7, True,  3),
    (2.0, 2.0, True,  1),
    (2.0, 1.7, False, 3),
    (2.0, 2.0, False, 1),
]

CELLS       = 4
DISCHARGE_A = 8.0
CHARGE_A    = 8.0
REST_S      = 90.0   # must exceed the largest evolvable full_rest_s


def make_genome(params=None):
    """Create a genome dict from params or random."""
    genome = {}
    for name, lo, hi, default in PARAM_DEFS:
        if params and name in params:
            genome[name] = params[name]
        else:
            genome[name] = random.uniform(lo, hi)
    return genome


def make_default_genome():
    return {name: default for name, lo, hi, default in PARAM_DEFS}


def clamp_genome(genome):
    for name, lo, hi, _ in PARAM_DEFS:
        genome[name] = max(lo, min(hi, genome[name]))
    return genome


def apply_genome(core, genome):
    """Write genome values into the core's bame_config_t."""
    cfg = core.config
    cfg.v_full_per_cell   = genome['v_full_per_cell']
    cfg.i_rest            = genome['i_rest']
    cfg.full_rest_ms      = int(genome['full_rest_s'] * 1000)
    cfg.v_rise_partial    = genome['v_rise_partial']
    cfg.v_disconnect_drop = genome['v_disconnect_drop']
    cfg.ext_rearm_ms      = int(genome['ext_rearm_s'] * 1000)


def evaluate(genome, seed, verbose=False):
    """Run all test cases, return fitness (lower = better).

    Endpoint metrics (learned capacity, final SOC) barely discriminate on
    clean deep cycles — almost any sane threshold set nails them. The
    mid-run SOC error is where thresholds bite (e.g. a FULL declared too
    early during a LOAD-mode charge snaps SOC to 100% while the pack is
    still filling), so it is sampled throughout and weighted in.
    """
    random.seed(seed)   # same sensor noise for every genome
    total = 0.0

    for true_ah, nominal_ah, wiring_bus, cycles in TEST_CASES:
        battery = Battery(true_capacity_ah=true_ah, cells=CELLS,
                          initial_soc=100.0,
                          voltage_noise=0.005, current_noise=0.005)
        core = BameCore(cells=CELLS, wiring_bus=wiring_bus,
                        capacity_ah=nominal_ah)
        apply_genome(core, genome)
        core.declare_full(0)   # boot: user declares full once

        # Mid-run SOC tracking error, sampled every 10th tick (1 s of sim
        # time) to keep the Python callback cost down.
        track = {'n': 0, 'err': 0.0, 'tick': 0}
        def on_tick(bat, c, track=track):
            track['tick'] += 1
            if track['tick'] % 10:
                return
            track['err'] += abs(bat.true_soc - c.soc_percent) / 100.0
            track['n'] += 1

        now_ms = 0
        n_full = 0
        for _ in range(cycles):
            battery.coulombs_remaining = battery.true_capacity_as
            events, now_ms = run_one_cycle(core, battery,
                                           DISCHARGE_A, CHARGE_A,
                                           REST_S, now_ms, on_tick=on_tick)
            n_full += sum(1 for _, e in events if e == EVT_FULL)

        # The run ends full and rested: learned capacity should match the
        # true one, and SOC should agree with the battery model.
        cap_err = abs(core.capacity_ah - true_ah) / true_ah
        soc_err = abs(battery.true_soc - core.soc_percent) / 100.0
        soc_track = track['err'] / track['n'] if track['n'] else 0.0
        penalty = 1.0 if n_full == 0 else 0.0   # never detected FULL at all

        total += 3.0 * cap_err + soc_err + 2.0 * soc_track + penalty

        if verbose:
            wiring = 'bus' if wiring_bus else 'load'
            status = 'OK' if n_full > 0 else 'NO-FULL'
            print(f"  {true_ah:.0f}Ah/nom{nominal_ah:.1f} {wiring:4s} x{cycles}: "
                  f"learned={core.capacity_ah:5.2f}Ah err={cap_err*100:5.1f}% "
                  f"soc_end={soc_err*100:4.1f}% soc_track={soc_track*100:4.1f}% "
                  f"fulls={n_full} [{status}]")

    return total / len(TEST_CASES)


def crossover(a, b):
    """Uniform crossover of two genomes."""
    child = {}
    for name, _, _, _ in PARAM_DEFS:
        child[name] = a[name] if random.random() < 0.5 else b[name]
    return child


def mutate(genome, rate=0.3, strength=0.2):
    """Mutate genome parameters."""
    g = copy.copy(genome)
    for name, lo, hi, _ in PARAM_DEFS:
        if random.random() < rate:
            span = hi - lo
            g[name] += random.gauss(0, span * strength)
    return clamp_genome(g)


def format_genome(genome):
    lines = []
    for name, lo, hi, default in PARAM_DEFS:
        val = genome[name]
        diff = ''
        if abs(val - default) / max(abs(default), 0.001) > 0.1:
            diff = f' (default {default})'
        lines.append(f"  {name:20s} = {val:8.3f}{diff}")
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Optimize BaMe v2 core thresholds (bame_config_t)')
    parser.add_argument('--generations', type=int, default=15)
    parser.add_argument('--population', type=int, default=20)
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()

    random.seed(args.seed)

    pop_size = args.population
    elite_size = max(2, pop_size // 5)

    # Initialize population: firmware defaults + variations + random
    population = [make_default_genome()]
    for _ in range(pop_size // 3):
        population.append(mutate(make_default_genome(), rate=0.5, strength=0.3))
    while len(population) < pop_size:
        population.append(make_genome())

    best_ever = None
    best_fitness = float('inf')

    print(f"=== BaMe v2 threshold optimizer (real C core) ===")
    print(f"Population: {pop_size}, Generations: {args.generations}, "
          f"Test cases: {len(TEST_CASES)}")
    print()

    default_fitness = evaluate(make_default_genome(), args.seed)
    print(f"Firmware-default fitness: {default_fitness:.4f}")
    print()

    for gen in range(args.generations):
        scored = []
        for idx, genome in enumerate(population):
            f = evaluate(genome, args.seed)
            scored.append((f, genome))
            done = gen * pop_size + idx + 1
            total = args.generations * pop_size
            print(f"\r  [{done}/{total} {done*100//total}%] "
                  f"Gen {gen+1} ind {idx+1}/{pop_size}", end='', flush=True)
        print('\r' + ' ' * 60 + '\r', end='')

        scored.sort(key=lambda x: x[0])

        if scored[0][0] < best_fitness:
            best_fitness = scored[0][0]
            best_ever = copy.copy(scored[0][1])

        avg = sum(f for f, _ in scored) / len(scored)
        print(f"Gen {gen+1:3d}: best={scored[0][0]:.4f} avg={avg:.4f} "
              f"best_ever={best_fitness:.4f}")

        # Selection + reproduction (GA state uses its own random stream;
        # evaluate() re-seeds internally so noise stays comparable).
        random.seed(args.seed + gen + 1)
        elites = [g for _, g in scored[:elite_size]]
        new_pop = list(elites)
        while len(new_pop) < pop_size:
            if random.random() < 0.7:
                child = crossover(random.choice(elites), random.choice(elites))
                child = mutate(child, rate=0.3, strength=0.15)
            else:
                child = mutate(random.choice(elites), rate=0.5, strength=0.25)
            new_pop.append(child)
        population = new_pop

    print()
    print(f"=== Best genome (fitness={best_fitness:.4f} "
          f"vs default={default_fitness:.4f}) ===")
    print(format_genome(best_ever))
    print()
    print(f"=== Detailed results with best genome ===")
    evaluate(best_ever, args.seed, verbose=True)
    print()
    print(f"=== Detailed results with firmware defaults ===")
    evaluate(make_default_genome(), args.seed, verbose=True)


if __name__ == '__main__':
    main()
