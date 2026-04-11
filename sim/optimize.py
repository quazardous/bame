#!/usr/bin/env python3
"""
Genetic optimizer for BAME calibration parameters.

Evolves the best parameter set by running the calibration simulation
across multiple scenarios and minimizing capacity estimation error.

Usage:
    python optimize.py [--generations 50] [--population 40]
"""

import argparse
import random
import copy
from calibration_sim import Battery, CalibrationState, SCENARIOS

# ============================================================
# Parameters to optimize (name, min, max, firmware default)
# ============================================================
PARAM_DEFS = [
    ('cal_initial_time_s',  10,    300,    60),
    ('cal_min_coulombs',    50,    2000,   500),
    ('rest_stable_s',       2,     30,     5),
    ('rest_current',        0.05,  1.0,    0.3),
    ('soc_blend_factor',    0.01,  0.20,   0.05),
    ('first_delta_soc_min', 0,     40,     30),
    ('converge_fast',       0.005, 0.05,   0.01),
    ('converge_slow',       0.0005,0.005,  0.001),
    ('charge_current',      0.3,   5.0,    1.0),
    ('charge_invalidate_s', 1,     30,     5),
]

# Test conditions: (true_capacity, nominal, cells, scenarios)
TEST_CASES = [
    (50,  80, 4, ['mixed', 'long', 'multicycle']),
    (80,  80, 4, ['mixed', 'long', 'multicycle']),
    (20,  80, 4, ['mixed', 'long']),
    (100, 80, 4, ['long', 'multicycle']),
]


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


def apply_params(cal, genome):
    """Apply genome parameters to a CalibrationState."""
    cal.CAL_INITIAL_TIME_S = genome['cal_initial_time_s']
    cal.CAL_MIN_COULOMBS = genome['cal_min_coulombs']
    cal.REST_STABLE_S = genome['rest_stable_s']
    cal.REST_CURRENT = genome['rest_current']
    cal.CONVERGE_FAST = genome['converge_fast']
    cal.CONVERGE_SLOW = genome['converge_slow']
    # These need custom handling in CalibrationState
    cal._soc_blend = genome['soc_blend_factor']
    cal._first_delta_min = genome['first_delta_soc_min']
    cal.CHARGE_CURRENT = genome['charge_current']
    cal._charge_inv_s = genome['charge_invalidate_s']


# Patch CalibrationState to use genome parameters
_orig_update = CalibrationState.update

def _patched_update(self, voltage, current_raw, dt, time_s):
    """Patched update that uses genome parameters."""
    current = current_raw - self.current_offset
    if abs(current) < 0.05:
        current = 0

    # Coulomb counting
    self.coulomb_count -= current * dt
    self.coulomb_count = max(0, min(self.estimated_as, self.coulomb_count))
    self.soc_percent = (self.coulomb_count / self.estimated_as) * 100.0

    # Calibration accumulation
    charge_inv_s = getattr(self, '_charge_inv_s', 5.0)
    if current > 0:
        self.cal_coulombs += current * dt
        self.cal_charge_sec = 0
    elif current < -getattr(self, 'CHARGE_CURRENT', 1.0):
        self.cal_charge_sec += dt
        if self.cal_charge_sec >= charge_inv_s:
            if self.cal_coulombs > 0 or self.cal_start_voltage > 0:
                self.log.append(f"  [{time_s:.0f}s] Segment invalidated")
            self.cal_coulombs = 0
            self.cal_target = 0
            self.cal_start_voltage = 0
            self.cal_charge_sec = 0
    else:
        self.cal_charge_sec = 0

    # Rest detection
    soc_blend = getattr(self, '_soc_blend', 0.05)
    if abs(current) < self.REST_CURRENT:
        if self.rest_since is None:
            self.rest_since = time_s
        stable_rest = (time_s - self.rest_since) >= self.REST_STABLE_S

        if stable_rest:
            soc_v = self.soc_from_voltage(voltage)
            self.soc_percent = self.soc_percent * (1 - soc_blend) + soc_v * soc_blend
            self.coulomb_count = (self.soc_percent / 100.0) * self.estimated_as

            raw = current + self.current_offset
            self.current_offset = self.current_offset * 0.9 + raw * 0.1

            # Voltage calibration
            if voltage > self.vbat_max * 0.98:
                conv = self.CONVERGE_FAST if voltage > self.vbat_max else self.CONVERGE_SLOW
                self.vbat_max = self.vbat_max * (1 - conv) + voltage * conv
            if voltage < self.vbat_min * 1.05:
                conv = self.CONVERGE_FAST if voltage < self.vbat_min else self.CONVERGE_SLOW
                self.vbat_min = self.vbat_min * (1 - conv) + voltage * conv

            if self.cal_start_voltage == 0:
                self.cal_start_voltage = voltage
                self.cal_start_time = time_s
    else:
        self.rest_since = None

    # Calibration save
    if self.cal_target <= 0:
        target_reached = ((time_s - self.cal_start_time) >= self.CAL_INITIAL_TIME_S
                          and self.cal_coulombs >= self.CAL_MIN_COULOMBS)
    else:
        target_reached = self.cal_coulombs >= self.cal_target

    first_delta_min = getattr(self, '_first_delta_min', 30.0)

    if (target_reached and self.cal_start_voltage > 0 and self.cal_coulombs > 0
            and abs(current) < self.REST_CURRENT
            and self.rest_since is not None
            and (time_s - self.rest_since) >= self.REST_STABLE_S):

        v_end = voltage
        soc_start = self.soc_from_voltage(self.cal_start_voltage)
        soc_end = self.soc_from_voltage(v_end)
        delta_soc = soc_start - soc_end

        if delta_soc > 5.0:
            est_ah = (self.cal_coulombs / 3600.0) / (delta_soc / 100.0)
            if 1.0 < est_ah < 500.0:
                accepted = False
                if not self.capacity_known:
                    if delta_soc > first_delta_min:
                        self.estimated_ah = est_ah
                        self.estimated_as = est_ah * 3600.0
                        self.capacity_known = True
                        accepted = True
                else:
                    weight = max(0.05, min(0.5, delta_soc / 100.0))
                    self.estimated_ah = self.estimated_ah * (1 - weight) + est_ah * weight
                    self.estimated_as = self.estimated_ah * 3600.0
                    accepted = True

                self.estimates.append((time_s, est_ah, delta_soc, self.estimated_ah, accepted))

        self.cal_last_delta_soc = int(delta_soc) if delta_soc > 0 else 0
        self.cal_target = self.cal_coulombs * 2.0
        self.cal_start_voltage = v_end
        self.cal_start_time = time_s
        self.cal_coulombs = 0


def evaluate(genome, verbose=False):
    """Run all test cases, return fitness (lower = better)."""
    total_error = 0
    total_tests = 0
    penalties = 0

    for true_ah, nominal_ah, cells, scenario_names in TEST_CASES:
        for scenario_name in scenario_names:
            dt = 0.1
            battery = Battery(true_ah, cells, voltage_noise=0.005)

            # Generate events
            battery_copy = Battery(true_ah, cells, voltage_noise=0.005)
            battery_copy.coulombs_remaining = battery.coulombs_remaining
            events = SCENARIOS[scenario_name](battery_copy, dt)
            battery.coulombs_remaining = battery.true_capacity_as * 0.75

            cal = CalibrationState(nominal_ah, cells)
            apply_params(cal, genome)

            # Monkey-patch update
            cal.update = lambda v, c, d, t, s=cal: _patched_update(s, v, c, d, t)

            v_init = battery.read_voltage(0)
            cal.soc_percent = cal.soc_from_voltage(v_init)
            cal.coulomb_count = (cal.soc_percent / 100.0) * cal.estimated_as

            for time_s, true_current in events:
                battery.discharge(true_current, dt)
                v = battery.read_voltage(true_current)
                i = battery.read_current(true_current)
                cal.update(v, i, dt, time_s)

            # Fitness components
            cap_error = abs(cal.estimated_ah - true_ah) / true_ah
            soc_error = abs(battery.true_soc - cal.soc_percent) / 100.0

            # Penalty if no calibration achieved
            if not cal.capacity_known:
                penalties += 1
                cap_error = abs(nominal_ah - true_ah) / true_ah  # worst case

            total_error += cap_error * 3 + soc_error  # weight capacity error more
            total_tests += 1

            if verbose:
                status = 'OK' if cal.capacity_known else 'FAIL'
                print(f"  {true_ah}Ah {scenario_name:12s}: "
                      f"est={cal.estimated_ah:5.1f}Ah err={cap_error*100:5.1f}% "
                      f"soc_err={soc_error*100:4.1f}% [{status}]")

    fitness = total_error / total_tests + penalties * 0.5
    return fitness


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
            diff = f' (was {default})'
        lines.append(f"  {name:25s} = {val:10.4f}{diff}")
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description='Optimize BAME calibration parameters')
    parser.add_argument('--generations', type=int, default=30, help='Number of generations')
    parser.add_argument('--population', type=int, default=30, help='Population size')
    parser.add_argument('--seed', type=int, default=42, help='Random seed')
    args = parser.parse_args()

    random.seed(args.seed)

    pop_size = args.population
    elite_size = max(2, pop_size // 5)

    # Initialize population with default + random
    population = [make_default_genome()]
    # Add variations around default
    for _ in range(pop_size // 3):
        population.append(mutate(make_default_genome(), rate=0.5, strength=0.3))
    # Fill rest with random
    while len(population) < pop_size:
        population.append(make_genome())

    best_ever = None
    best_fitness = float('inf')

    print(f"=== BAME Parameter Optimizer ===")
    print(f"Population: {pop_size}, Generations: {args.generations}")
    print(f"Test cases: {sum(len(tc[3]) for tc in TEST_CASES)} scenarios")
    print()

    # Evaluate default first
    default_fitness = evaluate(make_default_genome())
    print(f"Default firmware fitness: {default_fitness:.4f}")
    print()

    for gen in range(args.generations):
        # Evaluate
        scored = []
        for genome in population:
            random.seed(args.seed)  # deterministic evaluation
            f = evaluate(genome)
            scored.append((f, genome))

        scored.sort(key=lambda x: x[0])

        if scored[0][0] < best_fitness:
            best_fitness = scored[0][0]
            best_ever = copy.copy(scored[0][1])

        avg = sum(f for f, _ in scored) / len(scored)
        print(f"Gen {gen+1:3d}: best={scored[0][0]:.4f} avg={avg:.4f} "
              f"best_ever={best_fitness:.4f}")

        # Selection + reproduction
        elites = [g for _, g in scored[:elite_size]]
        new_pop = list(elites)

        while len(new_pop) < pop_size:
            if random.random() < 0.7:
                # Crossover two elites
                a = random.choice(elites)
                b = random.choice(elites)
                child = crossover(a, b)
                child = mutate(child, rate=0.3, strength=0.15)
            else:
                # Mutate an elite
                child = mutate(random.choice(elites), rate=0.5, strength=0.25)
            new_pop.append(child)

        population = new_pop

    # Final report
    print()
    print(f"=== Best parameters (fitness={best_fitness:.4f} vs default={default_fitness:.4f}) ===")
    print(format_genome(best_ever))
    print()
    print(f"=== Detailed results with best parameters ===")
    random.seed(args.seed)
    evaluate(best_ever, verbose=True)
    print()
    print(f"=== Detailed results with default parameters ===")
    random.seed(args.seed)
    evaluate(make_default_genome(), verbose=True)


if __name__ == '__main__':
    main()
