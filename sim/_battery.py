"""LFP battery model: OCV(SOC) curve, IR drop, polarization recovery, noise.

Used by trend_sim and calibration_sim. The model is intentionally simple but
captures the behaviors that matter for testing the firmware:

  - OCV varies with SOC along the LFP curve.
  - Voltage under load = OCV − I·R_internal − V_polar(t).
  - V_polar follows a first-order RC (τ ~ 30-60 s) so that after a load drop
    the voltage recovers logarithmically toward OCV — same shape as a real
    LFP rebound.
  - Sensor noise on V and I, plus a small INA-like current offset.
"""

import random


# Per-cell LFP SOC curve (matches the firmware table in main.cpp)
SOC_CURVE = [
    (3.65, 100), (3.40, 99), (3.35, 90), (3.325, 70),
    (3.30, 40), (3.275, 30), (3.25, 20), (3.20, 17),
    (3.00, 14), (2.75, 9), (2.50, 0),
]


def soc_from_voltage_percell(v_cell):
    if v_cell >= SOC_CURVE[0][0]:
        return 100.0
    if v_cell <= SOC_CURVE[-1][0]:
        return 0.0
    for i in range(len(SOC_CURVE) - 1):
        if v_cell >= SOC_CURVE[i + 1][0]:
            ratio = (v_cell - SOC_CURVE[i + 1][0]) / (
                SOC_CURVE[i][0] - SOC_CURVE[i + 1][0])
            return SOC_CURVE[i + 1][1] + ratio * (
                SOC_CURVE[i][1] - SOC_CURVE[i + 1][1])
    return 0.0


def voltage_from_soc_percell(soc):
    if soc >= 100:
        return SOC_CURVE[0][0]
    if soc <= 0:
        return SOC_CURVE[-1][0]
    for i in range(len(SOC_CURVE) - 1):
        if soc >= SOC_CURVE[i + 1][1]:
            ratio = (soc - SOC_CURVE[i + 1][1]) / (
                SOC_CURVE[i][1] - SOC_CURVE[i + 1][1])
            return SOC_CURVE[i + 1][0] + ratio * (
                SOC_CURVE[i][0] - SOC_CURVE[i + 1][0])
    return SOC_CURVE[-1][0]


class Battery:
    """LFP pack with realistic dynamics for closed-loop firmware tests."""

    def __init__(self, true_capacity_ah, cells=4, initial_soc=75.0,
                 r_internal_per_cell=0.005, r_polar_per_cell=0.010,
                 tau_polar_s=45.0, voltage_noise=0.005, current_noise=0.005,
                 current_offset=0.004):
        self.true_capacity_as = true_capacity_ah * 3600.0
        self.cells = cells
        self.coulombs_remaining = self.true_capacity_as * (initial_soc / 100.0)
        self.r_internal = r_internal_per_cell * cells
        self.r_polar = r_polar_per_cell * cells
        self.tau_polar = tau_polar_s
        self.voltage_noise = voltage_noise
        self.current_noise = current_noise
        self.current_offset = current_offset
        self.v_polar = 0.0  # voltage across the polarization RC

    @property
    def true_soc(self):
        return (self.coulombs_remaining / self.true_capacity_as) * 100.0

    def ocv(self):
        """Open-circuit voltage of the pack (no load, no noise, no polar)."""
        return voltage_from_soc_percell(self.true_soc) * self.cells

    def step(self, true_current, dt_s):
        """Advance battery state for dt seconds under the given load."""
        self.coulombs_remaining -= true_current * dt_s
        self.coulombs_remaining = max(
            0.0, min(self.true_capacity_as, self.coulombs_remaining))
        # Polarization (first-order): V_polar approaches I·R_polar with τ_polar.
        # When current drops to 0, V_polar decays exponentially → LFP rebound.
        if self.tau_polar > 0:
            target = true_current * self.r_polar
            self.v_polar += (target - self.v_polar) * (dt_s / self.tau_polar)

    def read_voltage(self, true_current):
        """Sensor reading: OCV − IR − V_polar + noise."""
        v = self.ocv() - true_current * self.r_internal - self.v_polar
        v += random.gauss(0, self.voltage_noise * self.cells)
        return v

    def read_current(self, true_current):
        """Sensor reading: true I + noise + INA226-like offset."""
        return true_current + random.gauss(0, self.current_noise) + self.current_offset
