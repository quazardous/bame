#include "bame_core.h"
#include <stdlib.h>  // for fabsf via <math.h> — we'll avoid it, see below

// Tiny utilities. Using our own abs/constrain so this file needs no libm.
static float f_absf(float x)             { return x < 0 ? -x : x; }
static float f_clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}


void bame_config_defaults(bame_config_t* cfg) {
    cfg->v_full_per_cell  = 3.40f;
    cfg->v_min_battery    = 1.0f;
    cfg->i_rest           = 0.3f;
    cfg->full_rest_ms     = 30000u;
    // EWMA τ ≈ 30 s at 100 ms tick → alpha = 0.1/30 ≈ 0.00333
    cfg->cavg_ewma_alpha  = 0.1f / 30.0f;
    cfg->cap_min_ah       = 1.0f;
    cfg->cap_max_ah       = 500.0f;
    cfg->v_rise_partial   = 0.05f;
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
    s->since_last_full_ms = 0u;
    s->rest_at_top_since_ms = 0u;
    s->v_slow_avg         = 0.0f;
    s->current_offset     = 0.0f;
    s->c_avg              = 0.0f;
    s->c_avg_init         = false;
    s->voltage            = 0.0f;
    s->current            = 0.0f;
}


float bame_capacity_as(const bame_state_t* s) {
    return s->capacity_ah * 3600.0f;
}


float bame_soc_percent(const bame_state_t* s) {
    float as = bame_capacity_as(s);
    if (as <= 0.0f) return 0.0f;
    return f_clampf(s->coulomb_count / as * 100.0f, 0.0f, 100.0f);
}


void bame_declare_full(bame_state_t* s, uint32_t now_ms) {
    s->coulomb_count         = bame_capacity_as(s);
    s->coulombs_at_last_full = s->coulomb_count;
    s->since_last_full_ms    = now_ms;
    s->soc_uncertain         = false;
    s->rest_at_top_since_ms  = 0u;
}


// BMS cutoff handler — Ah delivered since last full event = measured capacity.
// Blends into learned capacity (30% toward new sample).
static void handle_cutoff(bame_state_t* s, const bame_config_t* cfg) {
    if (s->coulombs_at_last_full <= 0.0f) return;  // never had a full reference
    float delivered_c = s->coulombs_at_last_full - s->coulomb_count;
    if (delivered_c <= 0.0f) return;
    float delivered_ah = delivered_c / 3600.0f;
    if (delivered_ah < cfg->cap_min_ah || delivered_ah > cfg->cap_max_ah) return;
    if (!s->capacity_learned) {
        s->capacity_ah      = delivered_ah;
        s->capacity_learned = true;
    } else {
        s->capacity_ah = s->capacity_ah * 0.70f + delivered_ah * 0.30f;
    }
    s->coulomb_count         = 0.0f;
    s->coulombs_at_last_full = 0.0f;  // marker: no reference until next full event
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

    // --- BMS cutoff: voltage collapsed → battery gone silent ---
    if (voltage_raw < cfg->v_min_battery) {
        bame_event_t evt = BAME_EVT_NONE;
        if (s->battery_present) {
            handle_cutoff(s, cfg);
            s->battery_present = false;
            evt = BAME_EVT_BMS_CUTOFF;
        }
        return evt;
    }

    // --- (Re)connection: fall back to nominal-full if the saved counter
    //     is obviously bogus. Flag uncertainty until a real event happens. ---
    if (!s->battery_present) {
        float cap_as = bame_capacity_as(s);
        if (s->coulomb_count <= 0.0f || s->coulomb_count > cap_as * 1.10f) {
            s->coulomb_count = cap_as;
            s->soc_uncertain = true;
        }
        s->battery_present = true;
        s->v_slow_avg      = voltage_raw;
    }

    // --- Coulomb integration (bidirectional: charge in BUS adds back) ---
    s->coulomb_count -= c * dt_s;
    float cap_as = bame_capacity_as(s);
    // Allow modest over/under-shoot so the cycle measurement sees real delta
    s->coulomb_count = f_clampf(s->coulomb_count, -cap_as * 0.10f, cap_as * 1.10f);

    // --- Slow current-offset auto-zero at low |I| ---
    if (f_absf(c) < cfg->i_rest) {
        float raw = c + s->current_offset;
        s->current_offset = s->current_offset * 0.99f + raw * 0.01f;
    }

    bame_event_t evt = BAME_EVT_NONE;

    // --- Full-event detection (BUS + LOAD, same trigger) ---
    bool at_top = (voltage_raw / (float)s->cells) >= cfg->v_full_per_cell
               && f_absf(c) < cfg->i_rest;
    if (at_top) {
        if (s->rest_at_top_since_ms == 0u) s->rest_at_top_since_ms = now_ms;
        if ((now_ms - s->rest_at_top_since_ms) >= cfg->full_rest_ms) {
            bame_declare_full(s, now_ms);
            evt = BAME_EVT_FULL;
        }
    } else {
        s->rest_at_top_since_ms = 0u;
    }

    // --- LOAD-mode partial-charge detection: unexplained V rise ---
    // In BUS mode, charge current is measurable so there is nothing invisible
    // and `soc_uncertain` is only touched by explicit resets/events.
    if (!s->wiring_bus) {
        if (voltage_raw - s->v_slow_avg > cfg->v_rise_partial) {
            if (!s->soc_uncertain) evt = BAME_EVT_PARTIAL;
            s->soc_uncertain = true;
        }
        s->v_slow_avg = s->v_slow_avg * 0.99f + voltage_raw * 0.01f;
    }

    return evt;
}
