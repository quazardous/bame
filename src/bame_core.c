#include "bame_core.h"
#include <stdlib.h>  // for fabsf via <math.h> — we'll avoid it, see below

// Tiny utilities. Using our own abs/constrain so this file needs no libm.
static float f_absf(float x)             { return x < 0 ? -x : x; }
static float f_clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

// LFP per-cell OCV→SOC curve (matches sim/_battery.py). Only the steep top and
// bottom regions are ever read for SOC (the flat middle is unusable), but the
// full table keeps the interpolation simple.
static float soc_from_v_percell(float v) {
    static const float V[] = {3.65f, 3.40f, 3.35f, 3.325f, 3.30f, 3.275f,
                              3.25f, 3.20f, 3.00f, 2.75f, 2.50f};
    static const float S[] = {100.0f, 99.0f, 90.0f, 70.0f, 40.0f, 30.0f,
                              20.0f, 17.0f, 14.0f, 9.0f, 0.0f};
    const int N = 11;
    if (v >= V[0]) return S[0];
    if (v <= V[N - 1]) return S[N - 1];
    for (int i = 0; i < N - 1; i++) {
        if (v >= V[i + 1]) {
            float r = (v - V[i + 1]) / (V[i] - V[i + 1]);
            return S[i + 1] + r * (S[i] - S[i + 1]);
        }
    }
    return S[N - 1];
}

// BUS two-anchor measurement: capacity from the Ah moved between the last
// anchor and this one, scaled by the SOC swing. Requires a meaningful swing
// (steep-region anchors only) so noise on either OCV read stays bounded.
#define BAME_MIN_DSOC 0.30f
static void bame__measure(bame_state_t* s, const bame_config_t* cfg, float soc_now) {
    float dsoc = f_absf(soc_now - s->soc_at_last_anchor) / 100.0f;
    if (dsoc > BAME_MIN_DSOC) {
        float span_ah = f_absf(s->coulomb_count - s->coulomb_at_last_anchor) / 3600.0f;
        float meas = span_ah / dsoc;   // extrapolate the swing to full 0–100 %
        if (meas >= cfg->cap_min_ah && meas <= cfg->cap_max_ah) {
            s->capacity_ah = (1.0f - cfg->cap_ewma_alpha) * s->capacity_ah
                           + cfg->cap_ewma_alpha * meas;
            s->capacity_learned = true;
        }
    }
}


void bame_config_defaults(bame_config_t* cfg) {
    cfg->v_full_per_cell  = 3.40f;
    cfg->i_rest           = 0.3f;
    cfg->full_rest_ms     = 30000u;
    // EWMA τ ≈ 30 s at 100 ms tick → alpha = 0.1/30 ≈ 0.00333
    cfg->cavg_ewma_alpha  = 0.1f / 30.0f;
    cfg->cap_min_ah       = 1.0f;
    cfg->cap_max_ah       = 500.0f;
    cfg->v_rise_partial   = 0.05f;
    cfg->v_disconnect_drop = 0.5f;
    cfg->ext_rearm_ms     = 15000u;
    cfg->v_knee_per_cell  = 3.05f;
    cfg->cap_ewma_alpha   = 0.50f;
}


void bame_init(bame_state_t* s, uint8_t cells, bool wiring_bus,
               float capacity_ah) {
    s->cells              = cells;
    s->wiring_bus         = wiring_bus;
    s->capacity_ah        = capacity_ah;
    s->capacity_learned   = false;
    s->coulomb_count      = capacity_ah * 3600.0f;  // assume nominal-full at boot
    s->soc_uncertain      = true;                   // until a real event happens
    s->battery_present    = false;
    s->coulombs_at_last_full = 0.0f;
    s->max_delivered_c_in_cycle = 0.0f;
    s->since_last_full_ms = 0u;
    s->rest_at_top_since_ms = 0u;
    s->v_slow_avg         = 0.0f;
    s->current_offset     = 0.0f;
    s->c_avg              = 0.0f;
    s->c_avg_init         = false;
    s->voltage            = 0.0f;
    s->current            = 0.0f;
    s->charging_external  = false;
    s->ext_charge_armed   = true;   // first ever "at top" can trigger the flag
    s->below_top_since_ms = 0u;
    s->last_anchor_kind   = 0u;
    s->coulomb_at_last_anchor = 0.0f;
    s->soc_at_last_anchor = 0.0f;
    s->rest_at_knee_since_ms = 0u;
    s->full_armed         = true;
    s->knee_armed         = true;
}


float bame_capacity_as(const bame_state_t* s) {
    return s->capacity_ah * 3600.0f;
}


float bame_soc_percent(const bame_state_t* s) {
    float as = bame_capacity_as(s);
    if (as <= 0.0f) return 0.0f;
    return f_clampf(s->coulomb_count / as * 100.0f, 0.0f, 100.0f);
}


// Amplitude-max learning: if this cycle's deepest discharge exceeded the
// current capacity, raise capacity to match. Monotonic (raise-only) — so
// BAME never overestimates SOC, only learns "this battery is at least this
// big". BAME runs on the very battery it measures, so the BMS cutoff is a
// hard power-loss event that the algorithm can never observe in prod — this
// FULL-event learning loop is the only viable convergence path.
void bame_declare_full(bame_state_t* s, const bame_config_t* cfg,
                       uint32_t now_ms) {
    // LOAD install: raise-only amplitude-max (charge is invisible, so there is
    // no bottom anchor to average against). BUS uses the two-anchor learner in
    // bame_step instead, which can also lower the estimate as the pack ages.
    if (!s->wiring_bus && s->max_delivered_c_in_cycle > 0.0f) {
        float observed_ah = s->max_delivered_c_in_cycle / 3600.0f;
        if (observed_ah > s->capacity_ah
                && observed_ah >= cfg->cap_min_ah
                && observed_ah <= cfg->cap_max_ah) {
            s->capacity_ah      = observed_ah;
            s->capacity_learned = true;
        }
    }
    s->max_delivered_c_in_cycle = 0.0f;
    s->coulomb_count         = bame_capacity_as(s);
    s->coulombs_at_last_full = s->coulomb_count;
    s->since_last_full_ms    = now_ms;
    s->soc_uncertain         = false;
    s->rest_at_top_since_ms  = 0u;
}


bame_event_t bame_step(bame_state_t* s, const bame_config_t* cfg,
                       float voltage_raw, float current_raw,
                       float dt_s, uint32_t now_ms) {
    // --- readVoltage / readCurrent analogue (offset + dead band) ---
    s->voltage = voltage_raw;
    float c = current_raw - s->current_offset;
    if (f_absf(c) < 0.05f) c = 0.0f;       // ±50 mA dead band
    s->current = c;

    // --- EWMA on smoothed current (display only, not gated on rest) ---
    if (!s->c_avg_init) {
        s->c_avg      = c;
        s->c_avg_init = true;
    } else {
        s->c_avg = cfg->cavg_ewma_alpha * c
                 + (1.0f - cfg->cavg_ewma_alpha) * s->c_avg;
    }

    // --- Battery presence + first-sight init ---
    // Presence tracks a plausible bus voltage. In prod BAME is powered by the
    // battery itself, so this stays true the whole time it runs (a BMS cutoff
    // removes power and reboots — it can't be observed as V→0 live). On the
    // bench, where the MCU is powered separately and nothing is on the shunt,
    // it correctly reads "no battery" so the splash shows.
    // MIN_BATTERY_V = 1.0 V — any real pack sits far above this even when flat.
    bool present = voltage_raw >= 1.0f;
    if (present && !s->battery_present) {
        // First sight of a real battery: seed the slow average and sanity-check
        // the restored counter. Only reset it when it's implausible — a normal
        // reconnect keeps the running count instead of snapping back to full.
        float cap_as = bame_capacity_as(s);
        if (s->coulomb_count <= 0.0f || s->coulomb_count > cap_as * 1.10f) {
            s->coulomb_count = cap_as;
            s->soc_uncertain = true;
        }
        s->v_slow_avg = voltage_raw;
    }
    s->battery_present = present;

    // --- External-charge detection (LOAD mode only). Freezes integration
    //     while the charger is holding the battery at top OCV. Symmetric
    //     hysteresis on both edges:
    //       * rapid DROP  (>0.5 V fast) = charger unplug → exit charging,
    //         stay out until a sustained below-top period re-arms
    //       * rapid RISE  (>0.5 V fast) = charger plug   → force re-arm,
    //         so the at-top condition immediately re-enters charging
    //         even if the battery was hanging at 13.8 V post-disconnect
    //         (no natural below-top dip needed)
    if (!s->wiring_bus) {
        // Slow-moving reference (tau ~50s at 100ms tick, alpha=0.002).
        s->v_slow_avg = s->v_slow_avg * 0.998f + voltage_raw * 0.002f;

        bool rapid_drop = (s->v_slow_avg - voltage_raw) > cfg->v_disconnect_drop;
        bool rapid_rise = (voltage_raw - s->v_slow_avg) > cfg->v_disconnect_drop;
        bool at_top     = (voltage_raw / (float)s->cells) >= cfg->v_full_per_cell;
        bool i_rest     = f_absf(c) < cfg->i_rest;

        // Rapid rise at-top = charger plugged in on top of a high battery.
        // Force re-arm so the at_top entry check below can fire immediately.
        if (rapid_rise && at_top) {
            s->ext_charge_armed = true;
            s->v_slow_avg = voltage_raw;  // stop re-firing
        }

        // Re-arm when voltage has stayed below the top plateau for at least
        // ext_rearm_ms (15 s default). Sustained condition filters out the
        // short LFP rebond right after a charger disconnect — voltage dips
        // briefly below top then recovers to 13.7-13.9 V. Only a real
        // discharge keeps V below top long enough.
        // below_top_since_ms stores (now_ms + 1) so 0 reliably means "not
        // tracking" even if at_top first becomes false at now_ms == 0.
        if (at_top) {
            s->below_top_since_ms = 0u;
        } else {
            if (s->below_top_since_ms == 0u) {
                s->below_top_since_ms = now_ms + 1u;
            }
            uint32_t below_duration = (now_ms + 1u) - s->below_top_since_ms;
            if (below_duration >= cfg->ext_rearm_ms) {
                s->ext_charge_armed = true;
            }
        }

        if (s->charging_external) {
            // In ext-charge state: fall out on rapid drop, current spike, or
            // voltage simply no longer at top.
            if (rapid_drop || !at_top || !i_rest) {
                s->charging_external = false;
                if (rapid_drop) s->v_slow_avg = voltage_raw;  // stop re-firing
            }
        } else {
            // Out of ext-charge: can enter only if armed AND conditions met.
            if (s->ext_charge_armed && at_top && i_rest) {
                s->charging_external = true;
                s->ext_charge_armed  = false;   // consumed
            }
        }
    } else {
        s->charging_external = false;
    }

    // Integration: suppressed while charger is driving the voltage.
    if (!s->charging_external) {
        s->coulomb_count -= c * dt_s;
    }
    float cap_as = bame_capacity_as(s);
    // Allow modest over/under-shoot so the cycle measurement sees real delta
    s->coulomb_count = f_clampf(s->coulomb_count, -cap_as * 0.10f, cap_as * 1.10f);

    // --- Amplitude-max tracking: peak depth-of-discharge since last FULL.
    //     Only meaningful once we have a FULL reference; otherwise the
    //     boot-time nominal-full assumption would inflate the measurement. ---
    if (s->coulombs_at_last_full > 0.0f) {
        float delivered_c = s->coulombs_at_last_full - s->coulomb_count;
        if (delivered_c > s->max_delivered_c_in_cycle) {
            s->max_delivered_c_in_cycle = delivered_c;
        }
    }

    // --- Slow current-offset auto-zero at low |I| ---
    if (f_absf(c) < cfg->i_rest) {
        float raw = c + s->current_offset;
        s->current_offset = s->current_offset * 0.99f + raw * 0.01f;
    }

    bame_event_t evt = BAME_EVT_NONE;

    // --- Full-event detection (BUS + LOAD, same trigger) ---
    float v_cell = voltage_raw / (float)s->cells;
    bool  rest   = f_absf(c) < cfg->i_rest;
    bool at_top = v_cell >= cfg->v_full_per_cell && rest;
    if (at_top) {
        if (s->rest_at_top_since_ms == 0u) s->rest_at_top_since_ms = now_ms;
        if (s->full_armed && (now_ms - s->rest_at_top_since_ms) >= cfg->full_rest_ms) {
            bame_declare_full(s, cfg, now_ms);   // resets coulomb (raise-only if LOAD)
            evt = BAME_EVT_FULL;
            // BUS: the FULL event only re-anchors the top and re-syncs SOC — it
            // does NOT measure capacity. Capacity is measured on the DISCHARGE
            // leg (FULL→KNEE) only; counting charge coulombs would overestimate
            // it because not all charge is retained (coulombic efficiency < 1).
            if (s->wiring_bus) {
                s->last_anchor_kind       = 1u;
                s->soc_at_last_anchor     = soc_from_v_percell(v_cell);
                s->coulomb_at_last_anchor = s->coulomb_count;  // post-reset = capacity
            }
            s->full_armed = false;   // one FULL per top visit
        }
    } else {
        s->rest_at_top_since_ms = 0u;
        if (v_cell < cfg->v_full_per_cell - 0.02f) s->full_armed = true;
    }

    // --- BUS KNEE-anchor detection (bottom of the LFP curve, at rest) ---
    // The second SOC anchor that makes learning bidirectional. LOAD can't use
    // it for the charge leg (charger invisible), so it stays BUS-only here.
    if (s->wiring_bus) {
        bool at_knee = rest && v_cell <= cfg->v_knee_per_cell;
        if (at_knee) {
            if (s->rest_at_knee_since_ms == 0u) s->rest_at_knee_since_ms = now_ms;
            if (s->knee_armed && (now_ms - s->rest_at_knee_since_ms) >= cfg->full_rest_ms) {
                float soc = soc_from_v_percell(v_cell);
                if (s->last_anchor_kind == 1u) bame__measure(s, cfg, soc);  // FULL→KNEE
                s->last_anchor_kind       = 2u;
                s->soc_at_last_anchor     = soc;
                s->coulomb_at_last_anchor = s->coulomb_count;
                s->knee_armed = false;   // one KNEE per bottom visit
            }
        } else {
            s->rest_at_knee_since_ms = 0u;
            if (v_cell > cfg->v_knee_per_cell + 0.02f) s->knee_armed = true;
        }
    }

    // --- LOAD-mode partial-charge detection: unexplained V rise ---
    // In BUS mode, charge current is measurable so nothing is invisible.
    // In LOAD, only trust a rise as "charger" when we're already above the
    // top-rest OCV — below that, rises are normal LFP rebound after a load.
    // Uses the same slow v_slow_avg as the disconnect detector above
    // (maintained there).
    if (!s->wiring_bus) {
        float v_charger_min = cfg->v_full_per_cell * (float)s->cells;
        if (voltage_raw >= v_charger_min
                && voltage_raw - s->v_slow_avg > cfg->v_rise_partial) {
            if (!s->soc_uncertain) evt = BAME_EVT_PARTIAL;
            s->soc_uncertain = true;
        }
    }

    return evt;
}
