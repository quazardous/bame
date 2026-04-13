#!/usr/bin/env python3
"""
BaMe Calibration Simulator

Simulates a LFP battery with known true capacity and runs the exact same
calibration algorithm as the firmware to validate convergence.

Usage:
    python calibration_sim.py [--capacity 50] [--nominal 80] [--cells 4]
                              [--noise 0.01] [--scenario mixed]
"""

import argparse
import random
import math

# ============================================================
# LFP per-cell SOC curve (same as firmware)
# ============================================================
SOC_CURVE = [
    (3.65, 100), (3.40, 99), (3.35, 90), (3.325, 70),
    (3.30, 40), (3.275, 30), (3.25, 20), (3.20, 17),
    (3.00, 14), (2.75, 9), (2.50, 0),
]


def soc_from_voltage_percell(v_cell):
    """SOC% from per-cell voltage using the LFP curve."""
    if v_cell >= SOC_CURVE[0][0]:
        return 100.0
    if v_cell <= SOC_CURVE[-1][0]:
        return 0.0
    for i in range(len(SOC_CURVE) - 1):
        if v_cell >= SOC_CURVE[i + 1][0]:
            ratio = (v_cell - SOC_CURVE[i + 1][0]) / (SOC_CURVE[i][0] - SOC_CURVE[i + 1][0])
            return SOC_CURVE[i + 1][1] + ratio * (SOC_CURVE[i][1] - SOC_CURVE[i + 1][1])
    return 0.0


def voltage_from_soc_percell(soc):
    """Per-cell voltage from SOC% (reverse lookup with interpolation)."""
    if soc >= 100:
        return SOC_CURVE[0][0]
    if soc <= 0:
        return SOC_CURVE[-1][0]
    for i in range(len(SOC_CURVE) - 1):
        if soc >= SOC_CURVE[i + 1][1]:
            ratio = (soc - SOC_CURVE[i + 1][1]) / (SOC_CURVE[i][1] - SOC_CURVE[i + 1][1])
            return SOC_CURVE[i + 1][0] + ratio * (SOC_CURVE[i][0] - SOC_CURVE[i + 1][0])
    return SOC_CURVE[-1][0]


# ============================================================
# Battery model
# ============================================================
class Battery:
    """Simulates a LFP battery pack with known true capacity."""

    def __init__(self, true_capacity_ah, cells, voltage_noise=0.01, current_noise=0.005):
        self.true_capacity_ah = true_capacity_ah
        self.true_capacity_as = true_capacity_ah * 3600.0
        self.cells = cells
        self.voltage_noise = voltage_noise  # V per cell
        self.current_noise = current_noise  # A
        self.coulombs_remaining = self.true_capacity_as * 0.75  # start at 75%
        self.internal_resistance = 0.005 * cells  # ~5mOhm per cell

    @property
    def true_soc(self):
        return (self.coulombs_remaining / self.true_capacity_as) * 100.0

    def ocv(self):
        """Open-circuit voltage (no load, no noise)."""
        soc = self.true_soc
        return voltage_from_soc_percell(soc) * self.cells

    def read_voltage(self, current=0):
        """Voltage under load with noise."""
        v = self.ocv() - current * self.internal_resistance
        v += random.gauss(0, self.voltage_noise * self.cells)
        return v

    def read_current(self, true_current):
        """Current with INA226-like noise + offset."""
        return true_current + random.gauss(0, self.current_noise) + 0.004  # 4mA offset

    def discharge(self, current, dt_seconds):
        """Apply discharge (current > 0) or charge (current < 0)."""
        self.coulombs_remaining -= current * dt_seconds
        self.coulombs_remaining = max(0, min(self.true_capacity_as, self.coulombs_remaining))


# ============================================================
# Firmware calibration algorithm (replicated from main.cpp)
# ============================================================
class CalibrationState:
    """Replicates the firmware calibration state machine."""

    CAL_INITIAL_TIME_S = 60  # 1 min
    CAL_MIN_COULOMBS = 500.0
    REST_CURRENT = 0.3
    CHARGE_CURRENT = 1.0
    REST_STABLE_S = 5.0
    CONVERGE_FAST = 0.01
    CONVERGE_SLOW = 0.001

    def __init__(self, nominal_ah, cells):
        self.cells = cells
        # Capacity
        self.nominal_ah = nominal_ah
        self.estimated_ah = nominal_ah
        self.estimated_as = nominal_ah * 3600.0
        self.capacity_trusted = False
        # Calibration segment
        self.cal_coulombs = 0
        self.cal_target = 0
        self.cal_start_voltage = 0
        self.cal_start_time = 0
        self.cal_charge_sec = 0
        self.cal_last_delta_soc = 0
        # Voltage calibration
        self.vbat_max = cells * 3.65
        self.vbat_min = cells * 2.50
        # Current offset
        self.current_offset = 0
        # SOC
        self.soc_percent = 50.0
        self.coulomb_count = nominal_ah * 3600.0 * 0.5  # start at 50%
        # Rest
        self.rest_since = None
        # Auto deep sleep
        self.auto_deep_sleep = False
        # History
        self.estimates = []
        self.log = []

    def soc_from_voltage(self, v):
        """Same as firmware socFromVoltage."""
        v_cell = v / self.cells
        v_min_cell = self.vbat_min / self.cells
        v_max_cell = self.vbat_max / self.cells
        if v_max_cell == v_min_cell:
            return 50.0
        v_scaled = 2.50 + (v_cell - v_min_cell) / (v_max_cell - v_min_cell) * (3.65 - 2.50)
        return soc_from_voltage_percell(v_scaled)

    def update(self, voltage, current_raw, dt, time_s):
        """One measurement cycle (replicates updateMeasurements)."""
        # Apply current offset
        current = current_raw - self.current_offset
        if abs(current) < 0.05:
            current = 0  # dead band

        # Coulomb counting
        self.coulomb_count -= current * dt
        self.coulomb_count = max(0, min(self.estimated_as, self.coulomb_count))
        self.soc_percent = (self.coulomb_count / self.estimated_as) * 100.0

        # Calibration accumulation
        if current > 0:
            self.cal_coulombs += current * dt
            self.cal_charge_sec = 0
        elif current < -self.CHARGE_CURRENT:
            self.cal_charge_sec += dt
            if self.cal_charge_sec >= 5.0:
                if self.cal_coulombs > 0 or self.cal_start_voltage > 0:
                    self.log.append(f"  [{time_s:.0f}s] Segment invalidated (charging)")
                self.cal_coulombs = 0
                self.cal_target = 0
                self.cal_start_voltage = 0
                self.cal_charge_sec = 0
        else:
            self.cal_charge_sec = 0

        # Rest detection
        if abs(current) < self.REST_CURRENT:
            if self.rest_since is None:
                self.rest_since = time_s
            stable_rest = (time_s - self.rest_since) >= self.REST_STABLE_S

            if stable_rest:
                # SOC blend (5%)
                soc_v = self.soc_from_voltage(voltage)
                self.soc_percent = self.soc_percent * 0.95 + soc_v * 0.05
                self.coulomb_count = (self.soc_percent / 100.0) * self.estimated_as

                # Current auto-zero
                raw = current + self.current_offset
                self.current_offset = self.current_offset * 0.9 + raw * 0.1

                # Voltage calibration (slow convergence)
                if voltage > self.vbat_max * 0.98:
                    conv = self.CONVERGE_FAST if voltage > self.vbat_max else self.CONVERGE_SLOW
                    self.vbat_max = self.vbat_max * (1 - conv) + voltage * conv
                if voltage < self.vbat_min * 1.05:
                    conv = self.CONVERGE_FAST if voltage < self.vbat_min else self.CONVERGE_SLOW
                    self.vbat_min = self.vbat_min * (1 - conv) + voltage * conv

                # Init segment start
                if self.cal_start_voltage == 0:
                    self.cal_start_voltage = voltage
                    self.cal_start_time = time_s
                    self.log.append(f"  [{time_s:.0f}s] Segment start: {voltage:.3f}V")
        else:
            self.rest_since = None

        # Calibration save check
        if self.cal_target <= 0:
            target_reached = ((time_s - self.cal_start_time) >= self.CAL_INITIAL_TIME_S
                              and self.cal_coulombs >= self.CAL_MIN_COULOMBS)
        else:
            target_reached = self.cal_coulombs >= self.cal_target

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
                    # Unified weighted convergence
                    weight = max(0.05, min(0.5, delta_soc / 100.0))
                    self.estimated_ah = self.estimated_ah * (1 - weight) + est_ah * weight
                    self.estimated_as = self.estimated_ah * 3600.0
                    # Trust once moved >5% from nominal
                    if not self.capacity_trusted and abs(self.estimated_ah - self.nominal_ah) > self.nominal_ah * 0.05:
                        self.capacity_trusted = True
                        self.auto_deep_sleep = True

                    self.estimates.append((time_s, est_ah, delta_soc, self.estimated_ah, True))
                    trusted = 'T' if self.capacity_trusted else ''
                    self.log.append(
                        f"  [{time_s:.0f}s] CAL: {self.cal_coulombs/3600:.1f}Ah "
                        f"dSOC={delta_soc:.1f}% w={weight:.2f} est={est_ah:.1f}Ah "
                        f"-> cap={self.estimated_ah:.1f}Ah {trusted}"
                    )

            self.cal_last_delta_soc = int(delta_soc)
            self.cal_target = self.cal_coulombs * 2.0
            self.cal_start_voltage = v_end
            self.cal_start_time = time_s
            self.cal_coulombs = 0


# ============================================================
# Scenarios
# ============================================================
def scenario_steady(battery, dt=0.1):
    """Steady discharge at 5A until 20%, then rest."""
    events = []
    t = 0
    # Initial rest (10s)
    for _ in range(int(10 / dt)):
        events.append((t, 0))
        t += dt
    # Discharge at 5A
    while battery.true_soc > 20:
        events.append((t, 5.0))
        t += dt
        battery.discharge(5.0, dt)
    # Reset battery state for actual sim
    battery.coulombs_remaining = battery.true_capacity_as * 0.75
    # Final rest (30s)
    for _ in range(int(30 / dt)):
        events.append((t, 0))
        t += dt
    return events


def scenario_mixed(battery, dt=0.1):
    """Realistic van scenario: discharge, rest, partial charge, repeat."""
    events = []
    t = 0

    def add(current, duration):
        nonlocal t
        for _ in range(int(duration / dt)):
            events.append((t, current))
            t += dt

    # Start with rest
    add(0, 15)

    # Cycle 1: fridge running (2A for 30min), then rest
    add(2.0, 1800)
    add(0, 20)

    # Cycle 2: heavier load (8A for 15min), then rest
    add(8.0, 900)
    add(0, 20)

    # Partial charge from solar (3A for 5min) — should invalidate segment
    add(-3.0, 300)
    add(0, 15)

    # Cycle 3: mixed load (5A for 45min), then rest
    add(5.0, 2700)
    add(0, 20)

    # Cycle 4: light load (1A for 60min), then rest
    add(1.0, 3600)
    add(0, 20)

    # Cycle 5: heavy load (10A for 20min), then rest
    add(10.0, 1200)
    add(0, 30)

    return events


def scenario_long(battery, dt=0.1):
    """Long discharge from 90% to 10% at 3A with periodic rests."""
    events = []
    t = 0

    def add(current, duration):
        nonlocal t
        for _ in range(int(duration / dt)):
            events.append((t, current))
            t += dt

    add(0, 15)  # initial rest

    # Discharge in 30min blocks with 15s rest between
    for _ in range(20):
        add(3.0, 1800)
        add(0, 15)

    add(0, 30)  # final rest
    return events


def scenario_multicycle(battery, dt=0.1):
    """Multiple charge/discharge cycles to test convergence."""
    events = []
    t = 0

    def add(current, duration):
        nonlocal t
        for _ in range(int(duration / dt)):
            events.append((t, current))
            t += dt

    for cycle in range(4):
        add(0, 15)  # rest
        # Discharge at 5A for 2h (10Ah per cycle)
        for _ in range(8):  # 8 x 15min blocks
            add(5.0, 900)
            add(0, 15)
        # Charge at 10A for 1h (back ~10Ah)
        add(-10.0, 3600)
        add(0, 15)

    # Final long discharge
    add(0, 15)
    for _ in range(12):
        add(3.0, 1800)
        add(0, 15)
    add(0, 30)

    return events


SCENARIOS = {
    'steady': scenario_steady,
    'mixed': scenario_mixed,
    'long': scenario_long,
    'multicycle': scenario_multicycle,
}


# ============================================================
# Main simulation
# ============================================================
def run_simulation(true_ah, nominal_ah, cells, noise, scenario_name):
    dt = 0.1  # 100ms like firmware

    battery = Battery(true_ah, cells, voltage_noise=noise)
    cal = CalibrationState(nominal_ah, cells)

    # Generate scenario events
    battery_copy = Battery(true_ah, cells, voltage_noise=noise)
    battery_copy.coulombs_remaining = battery.coulombs_remaining
    scenario_fn = SCENARIOS[scenario_name]
    events = scenario_fn(battery_copy, dt)

    # Reset battery to 75%
    battery.coulombs_remaining = battery.true_capacity_as * 0.75

    # Init SOC from voltage (like firmware boot)
    v_init = battery.read_voltage(0)
    cal.soc_percent = cal.soc_from_voltage(v_init)
    cal.coulomb_count = (cal.soc_percent / 100.0) * cal.estimated_as

    print(f"=== BaMe Calibration Simulation ===")
    print(f"True capacity:    {true_ah} Ah")
    print(f"Nominal capacity: {nominal_ah} Ah")
    print(f"Cells:            {cells}S")
    print(f"Voltage noise:    {noise*1000:.0f}mV/cell")
    print(f"Scenario:         {scenario_name}")
    print(f"Duration:         {events[-1][0]:.0f}s ({events[-1][0]/3600:.1f}h)")
    print(f"Initial SOC:      {battery.true_soc:.1f}% (true) / {cal.soc_percent:.1f}% (estimated)")
    print(f"---")

    last_print = -999
    for time_s, true_current in events:
        # Battery physics
        battery.discharge(true_current, dt)

        # Sensor readings (with noise)
        v = battery.read_voltage(true_current)
        i = battery.read_current(true_current)

        # Firmware algorithm
        old_log_len = len(cal.log)
        cal.update(v, i, dt, time_s)

        # Print new log entries
        for entry in cal.log[old_log_len:]:
            print(entry)

    print(f"---")
    print(f"Final true SOC:   {battery.true_soc:.1f}%")
    print(f"Final est. SOC:   {cal.soc_percent:.1f}%")
    print(f"SOC error:        {abs(battery.true_soc - cal.soc_percent):.1f}%")
    print()
    print(f"True capacity:    {true_ah:.1f} Ah")
    print(f"Est. capacity:    {cal.estimated_ah:.1f} Ah")
    print(f"Error:            {abs(true_ah - cal.estimated_ah):.1f} Ah ({abs(true_ah - cal.estimated_ah)/true_ah*100:.1f}%)")
    print(f"Capacity trusted:  {cal.capacity_trusted}")
    print(f"Eco mode:         {cal.auto_deep_sleep}")
    print(f"Current offset:   {cal.current_offset*1000:.1f}mA")
    print(f"Vmax/Vmin:        {cal.vbat_max:.3f}/{cal.vbat_min:.3f}V")
    print()

    if cal.estimates:
        print(f"Calibration history ({len(cal.estimates)} estimates):")
        print(f"  {'Time':>8s}  {'Raw est':>8s}  {'dSOC':>5s}  {'Cap now':>8s}  {'Error':>6s}  Status")
        for t, est, ds, cap, ok in cal.estimates:
            err = abs(est - true_ah) / true_ah * 100
            status = 'OK' if ok else 'rejected'
            print(f"  {t:7.0f}s  {est:7.1f}Ah  {ds:4.1f}%  {cap:7.1f}Ah  {err:5.1f}%  {status}")
    else:
        print("No calibration estimates produced.")
        print("(Scenario may be too short or lack sufficient rest periods)")

    return cal


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='BaMe calibration simulator')
    parser.add_argument('--capacity', type=float, default=50, help='True battery capacity in Ah')
    parser.add_argument('--nominal', type=float, default=80, help='Nominal (user-set) capacity in Ah')
    parser.add_argument('--cells', type=int, default=4, help='Cell count (S)')
    parser.add_argument('--noise', type=float, default=0.005, help='Voltage noise per cell in V')
    parser.add_argument('--scenario', choices=SCENARIOS.keys(), default='mixed', help='Test scenario')
    parser.add_argument('--seed', type=int, default=42, help='Random seed')
    args = parser.parse_args()

    random.seed(args.seed)
    run_simulation(args.capacity, args.nominal, args.cells, args.noise, args.scenario)
