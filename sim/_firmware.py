"""Faithful Python mirror of src/main.cpp v1.17 measurement & calibration logic.

One class — `FirmwareMirror` — exposes everything the firmware does on each
100 ms tick:

  - readVoltage / readCurrent (with currentOffset and ±50 mA dead band)
  - cAvg EWMA (smoothed current, τ from CAVG_EWMA_TAU_S)
  - coulombCount (clamped, SOC-corrected) and coulombRaw (pure integrator)
  - calCoulombs accumulation + sustained-charge invalidation
  - 8-sample voltage + coulombRaw buffer (every 10 s)
  - LR slope → voltageTrend, externalChargeDetected hysteresis
  - maxSliceI from buffer + LIVE |current| → flatNow gate
  - flatSince chrono with retro-range backdating (dev mode)
  - Optimistic segment open + discard (dev)
  - SOC blend, currentOffset auto-zero, vbatTop slow convergence on stableRest
  - Preempt / commit / rollback segment end (dev)
    OR single-shot save (prod)

Two configuration knobs:
  - `dev=True`  : full chrono + preempt/rollback path
  - `dev=False` : prod fallback (direct gate, single-shot save)
"""

import math

from _battery import soc_from_voltage_percell, SOC_CURVE


# === Firmware constants (from src/main.cpp) ===
TICK_S = 0.1
MEASURE_INTERVAL_S = 0.1

VBAT_REST_CURRENT   = 0.3
VBAT_CHARGE_CURRENT = 1.0
ACTIVE_CURRENT      = 0.5

LFP_CELL_FULL       = 3.65
LFP_CELL_EMPTY      = 2.50
LFP_CELL_CHARGE     = 3.40
LFP_CELL_CHARGE_MIN = 3.375

VBAT_CONVERGE_FAST = 0.04
VBAT_CONVERGE_SLOW = 0.001

VHIST_SIZE            = 8
VHIST_INTERVAL_S      = 10.0
VHIST_SLOPE_THRESHOLD = 0.005   # V per 10s step
FLAT_COUNTDOWN_S      = 60.0
FLAT_RETRO_SPREAD     = 0.020   # V

CAVG_EWMA_TAU_S = 30.0          # current default in firmware
CAVG_EWMA_ALPHA = TICK_S / CAVG_EWMA_TAU_S

CAL_INITIAL_TIME_S = 120.0
CAL_MIN_COULOMBS   = 500.0

CAPACITY_MIN = 1.0
CAPACITY_MAX = 500.0

# Linear regression denominators for x = 0..VHIST_SIZE-1 (compile-time folded)
def _lr_constants(N):
    Sx = N * (N - 1) // 2
    Sxx = N * (N - 1) * (2 * N - 1) // 6
    return Sx, N * Sxx - Sx * Sx
VHIST_SX, VHIST_D = _lr_constants(VHIST_SIZE)


def soc_from_voltage(v_pack, cells, vbat_bottom, vbat_top):
    """Same shape as firmware socFromVoltage: rescale into [2.50..3.65] cell range."""
    v_cell = v_pack / cells
    v_min_cell = vbat_bottom / cells
    v_max_cell = vbat_top / cells
    if v_max_cell == v_min_cell:
        return 50.0
    v_scaled = LFP_CELL_EMPTY + (v_cell - v_min_cell) / (v_max_cell - v_min_cell) * (
        LFP_CELL_FULL - LFP_CELL_EMPTY)
    return soc_from_voltage_percell(v_scaled)


class FirmwareMirror:
    """Stateful mirror of the v1.17 firmware updateMeasurements pipeline."""

    def __init__(self, nominal_capacity_ah=80.0, cells=4,
                 v_min_utile=12.0, v_max_utile=13.6, dev=True):
        self.cells = cells
        self.dev = dev

        # Capacity (estimated, may be revised by calibration)
        self.battery_capacity_ah = nominal_capacity_ah
        self.battery_capacity_as = nominal_capacity_ah * 3600.0

        # Voltage calibration window (top auto-converges, bottom user-set)
        self.vbat_top = cells * LFP_CELL_FULL
        self.vbat_bottom = cells * LFP_CELL_EMPTY
        self.v_min_utile = v_min_utile
        self.v_max_utile = v_max_utile

        # Live measurements
        self.voltage = 0.0
        self.current = 0.0          # corrected (after offset + dead band)
        self.current_offset = 0.0
        self.cAvg = 0.0
        self._cAvg_init = False

        # Coulomb integrators
        self.soc_percent = 50.0
        self.coulomb_count = 0.0
        self.coulomb_raw = 0.0      # pure, no clamp, no SOC blend

        # Calibration state
        self.cal_coulombs = 0.0
        self.cal_charge_sec = 0.0
        self.cal_target = 0.0
        self.cal_start_voltage = 0.0
        self.cal_start_time = 0.0

        # Preempt state (dev)
        self.pending_end_voltage = 0.0
        self.pending_end_coulombs = 0.0
        self.pending_end_time = 0.0

        # Buffer
        self.vhist = [0.0] * VHIST_SIZE
        self.chist = [0.0] * VHIST_SIZE
        self.vhist_idx = 0
        self.vhist_count = 0
        self.last_push_t = -1.0

        # Derived from buffer
        self.buf_min = 0.0
        self.voltage_trend = 0
        self.max_slice_i = 0.0
        self.external_charge_detected = False
        self.flat_since = 0.0

        # Battery present flag (mirror of static bool in firmware)
        self.battery_present = False

        # History — populated as side effect for caller inspection
        self.cap_estimates = []     # list of (t, est, deltaSoc, capacity_after)
        self.events = []            # human-readable log of state transitions

    # --- public API ---

    def init_soc_from_voltage(self, voltage):
        """Mirror of the boot-time SOC sync."""
        self.soc_percent = soc_from_voltage(
            voltage, self.cells, self.vbat_bottom, self.vbat_top)
        self.coulomb_count = (self.soc_percent / 100.0) * self.battery_capacity_as
        self.coulomb_raw = self.coulomb_count

    def reset_calibration(self):
        self.cal_coulombs = 0.0
        self.cal_target = 0.0
        self.cal_start_voltage = 0.0
        self.pending_end_coulombs = 0.0

    def set_capacity(self, ah):
        self.battery_capacity_ah = ah
        self.battery_capacity_as = ah * 3600.0

    def update(self, voltage_raw, current_raw, t):
        """One tick. `t` is seconds since start (mirrors millis()/1000)."""
        # readVoltage / readCurrent (offset + dead band)
        self.voltage = voltage_raw
        c = current_raw - self.current_offset
        if abs(c) < 0.05:
            c = 0.0
        self.current = c

        # cAvg EWMA at every tick
        if not self._cAvg_init:
            self.cAvg = self.current
            self._cAvg_init = True
        else:
            self.cAvg = (CAVG_EWMA_ALPHA * self.current
                         + (1.0 - CAVG_EWMA_ALPHA) * self.cAvg)

        # Battery presence (1V threshold like firmware MIN_BATTERY_V)
        if self.voltage < 1.0:
            self.battery_present = False
            self.soc_percent = 0.0
            self.coulomb_count = 0.0
            return
        if not self.battery_present:
            self.init_soc_from_voltage(self.voltage)
            if self.voltage >= self.vbat_bottom:
                self.battery_present = True
            return

        # Coulomb counting
        self.coulomb_count -= self.current * TICK_S
        self.coulomb_raw   -= self.current * TICK_S
        self.coulomb_count = max(0.0, min(self.battery_capacity_as, self.coulomb_count))
        self.soc_percent = (self.coulomb_count / self.battery_capacity_as) * 100.0

        # Calibration accumulation + sustained-charge invalidation
        if self.current > 0:
            self.cal_coulombs += self.current * TICK_S
            self.cal_charge_sec = 0.0
        elif self.current < -VBAT_CHARGE_CURRENT:
            self.cal_charge_sec += TICK_S
            if self.cal_charge_sec >= 10.0:
                self.reset_calibration()
                self.cal_charge_sec = 0.0
        else:
            self.cal_charge_sec = 0.0

        # Buffer push every VHIST_INTERVAL_S
        if self.last_push_t < 0 or (t - self.last_push_t) >= VHIST_INTERVAL_S:
            # Renormalize coulombRaw if it drifts past ±100k
            if self.coulomb_raw > 100000.0 or self.coulomb_raw < -100000.0:
                shift = self.coulomb_raw
                for i in range(VHIST_SIZE):
                    self.chist[i] -= shift
                self.coulomb_raw = 0.0
            self.vhist[self.vhist_idx] = self.voltage
            self.chist[self.vhist_idx] = self.coulomb_raw
            self.vhist_idx = (self.vhist_idx + 1) % VHIST_SIZE
            if self.vhist_count < VHIST_SIZE:
                self.vhist_count += 1
            self.last_push_t = t

        # Buffer analysis (when full)
        if self.vhist_count == VHIST_SIZE:
            start_idx = self.vhist_idx  # oldest
            ordered_v = [self.vhist[(start_idx + i) % VHIST_SIZE] for i in range(VHIST_SIZE)]
            ordered_c = [self.chist[(start_idx + i) % VHIST_SIZE] for i in range(VHIST_SIZE)]
            sum_y = sum(ordered_v)
            sum_iy = sum(i * v for i, v in enumerate(ordered_v))
            self.buf_min = min(ordered_v)
            slope_v = (VHIST_SIZE * sum_iy - VHIST_SX * sum_y) / VHIST_D
            self.voltage_trend = (1 if slope_v > VHIST_SLOPE_THRESHOLD else
                                  -1 if slope_v < -VHIST_SLOPE_THRESHOLD else 0)
            if self.voltage_trend > 0:
                self.external_charge_detected = True
            elif self.voltage_trend < 0:
                self.external_charge_detected = False
            self.max_slice_i = 0.0
            for i in range(1, VHIST_SIZE):
                slice_i = abs(ordered_c[i] - ordered_c[i - 1]) / VHIST_INTERVAL_S
                if slice_i > self.max_slice_i:
                    self.max_slice_i = slice_i

        # Phantom-charger guard: rejects voltage-trend-only signal when the
        # voltage is too low (likely an LFP rebound, not a real charger).
        # Skipped when shunt current confirms a real charge in progress.
        if (self.voltage < self.cells * LFP_CELL_CHARGE_MIN
                and self.current >= -VBAT_REST_CURRENT):
            self.external_charge_detected = False
        # Through-shunt charge: sustained negative current is unambiguous.
        if (self.current <= -VBAT_CHARGE_CURRENT
                and self.cal_charge_sec >= 5.0):
            self.external_charge_detected = True

        if self.external_charge_detected:
            self.reset_calibration()

        # Flat chrono + retro-range backdating (dev only)
        if self.dev:
            if self.voltage_trend != 0 or self.buf_min == 0:
                self.flat_since = t
            elif self.vhist_count == VHIST_SIZE:
                # Retro-range scan from newest
                newest_idx = (self.vhist_idx + VHIST_SIZE - 1) % VHIST_SIZE
                t_min = self.vhist[newest_idx]
                t_max = t_min
                flat_count = 1
                for back in range(1, VHIST_SIZE):
                    idx = (newest_idx + VHIST_SIZE - back) % VHIST_SIZE
                    v = self.vhist[idx]
                    if v < t_min: t_min = v
                    if v > t_max: t_max = v
                    if t_max - t_min > FLAT_RETRO_SPREAD:
                        break
                    flat_count += 1
                backdated = t - (flat_count - 1) * VHIST_INTERVAL_S
                if backdated < self.flat_since:
                    self.flat_since = backdated

        # Rest gate
        flat_now = (self.buf_min > 0
                    and self.max_slice_i < VBAT_REST_CURRENT
                    and abs(self.current) < VBAT_REST_CURRENT
                    and self.voltage_trend == 0)
        if self.dev:
            stable_rest = flat_now and (t - self.flat_since >= FLAT_COUNTDOWN_S)
            # Optimistic segment open
            if (flat_now and not self.external_charge_detected
                    and self.cal_start_voltage == 0):
                self.cal_start_voltage = self.voltage
                self.cal_start_time = t
                self.events.append((t, "OPEN_OPTIMISTIC", self.voltage))
            # Discard optimistic open
            if (self.voltage_trend != 0 and self.cal_start_voltage > 0
                    and self.cal_coulombs == 0
                    and t - self.flat_since < FLAT_COUNTDOWN_S):
                self.cal_start_voltage = 0
                self.events.append((t, "DISCARD_OPTIMISTIC", None))
        else:
            stable_rest = flat_now

        if stable_rest and not self.external_charge_detected:
            soc_v = soc_from_voltage(
                self.voltage, self.cells, self.vbat_bottom, self.vbat_top)
            self.soc_percent = self.soc_percent * 0.92 + soc_v * 0.08
            self.coulomb_count = (self.soc_percent / 100.0) * self.battery_capacity_as
            # Auto-zero current offset
            raw = self.current + self.current_offset
            self.current_offset = self.current_offset * 0.9 + raw * 0.1
            # Prod-only segment open
            if not self.dev and self.cal_start_voltage == 0:
                self.cal_start_voltage = self.voltage
                self.cal_start_time = t
                self.events.append((t, "OPEN", self.voltage))
            # vbat_top slow convergence
            if (self.current >= -VBAT_REST_CURRENT
                    and self.voltage > self.vbat_top * 0.98):
                conv = (VBAT_CONVERGE_FAST if self.voltage > self.vbat_top
                        else VBAT_CONVERGE_SLOW)
                self.vbat_top = self.vbat_top * (1.0 - conv) + self.voltage * conv

        # Segment-end target check
        if self.cal_target <= 0:
            cal_target_reached = (
                (t - self.cal_start_time) >= CAL_INITIAL_TIME_S
                and self.cal_coulombs >= CAL_MIN_COULOMBS)
        else:
            cal_target_reached = self.cal_coulombs >= self.cal_target

        if self.dev:
            # Preempt
            if (cal_target_reached and self.cal_start_voltage > 0
                    and self.cal_coulombs > 0
                    and flat_now and not self.external_charge_detected
                    and self.pending_end_coulombs == 0):
                self.pending_end_voltage = self.voltage
                self.pending_end_coulombs = self.cal_coulombs
                self.pending_end_time = t
                self.cal_coulombs = 0.0
                self.events.append((t, "PREEMPT", self.voltage))
            # Rollback
            if (self.pending_end_coulombs > 0 and self.voltage_trend != 0
                    and t - self.flat_since < FLAT_COUNTDOWN_S):
                self.cal_coulombs += self.pending_end_coulombs
                self.pending_end_coulombs = 0.0
                self.events.append((t, "ROLLBACK", None))
            # Commit
            if (self.pending_end_coulombs > 0 and stable_rest
                    and not self.external_charge_detected):
                self._commit_segment(
                    t, self.cal_start_voltage, self.pending_end_voltage,
                    self.pending_end_coulombs, self.pending_end_time)
                self.pending_end_coulombs = 0.0
        else:
            # Single-shot save (prod)
            if (cal_target_reached and self.cal_start_voltage > 0
                    and self.cal_coulombs > 0 and stable_rest):
                self._commit_segment(
                    t, self.cal_start_voltage, self.voltage,
                    self.cal_coulombs, t)
                self.cal_coulombs = 0.0

    def _commit_segment(self, t, v_start, v_end, coulombs, end_time):
        soc_start = soc_from_voltage(
            v_start, self.cells, self.vbat_bottom, self.vbat_top)
        soc_end = soc_from_voltage(
            v_end, self.cells, self.vbat_bottom, self.vbat_top)
        delta_soc = soc_start - soc_end
        accepted = False
        est_ah = None
        if delta_soc > 5.0:
            est_ah = (coulombs / 3600.0) / (delta_soc / 100.0)
            if CAPACITY_MIN < est_ah < CAPACITY_MAX:
                weight = max(0.05, min(0.5, delta_soc / 100.0))
                self.set_capacity(
                    self.battery_capacity_ah * (1.0 - weight) + est_ah * weight)
                accepted = True
        self.cap_estimates.append((t, est_ah, delta_soc,
                                    self.battery_capacity_ah, accepted))
        self.cal_target = coulombs * 2.0
        self.cal_start_voltage = v_end
        self.cal_start_time = end_time
        self.events.append((t, "COMMIT" if accepted else "REJECT",
                            (est_ah, delta_soc, self.battery_capacity_ah)))
