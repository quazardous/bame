// BaMe core algorithm — platform-agnostic C.
//
// Single source of truth for the v2 measurement + event-detection logic.
// Compiled into the firmware (called from main.cpp) AND into a host-side
// shared library for sim/ (loaded via ctypes from Python).
//
// No Arduino / AVR / hardware dependencies. Pure C, bool + stdint only.

#ifndef BAME_CORE_H
#define BAME_CORE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    BAME_EVT_NONE    = 0,
    BAME_EVT_FULL    = 1,   // battery full event (SOC → 100%)
    BAME_EVT_PARTIAL = 3,   // LOAD-mode unexplained V rise (soc_uncertain set)
} bame_event_t;


// Tunable thresholds. Filled by `bame_config_defaults()` to match firmware.
typedef struct {
    float    v_full_per_cell;   // 3.40  — rest voltage above this = at-top
    float    i_rest;            // 0.3   — |I| below this = battery at rest
    uint32_t full_rest_ms;       // 30000 — sustained top+rest needed for full evt
    float    cavg_ewma_alpha;    // 0.1/30 — smoothed current constant
    float    cap_min_ah;         // 1.0   — sanity bounds for learned capacity
    float    cap_max_ah;         // 500.0
    float    v_rise_partial;     // 0.05  — LOAD mode: V rise > this = partial charge
    float    v_disconnect_drop;  // 0.5   — LOAD mode: V drop > this (fast) = charger unplug
    uint32_t ext_rearm_ms;       // 15000 — V must stay below top this long to re-arm
                                 //          (filters out post-disconnect LFP rebond)
    // BUS-only bidirectional (aging-aware) capacity learning:
    float    v_knee_per_cell;    // 3.05  — rest V/cell at/below this = KNEE anchor
    float    cap_ewma_alpha;     // 0.50  — capacity EWMA weight per discharge measure
    // Slow current average driving the autonomy estimate. τ ≈ 1 h so an
    // intermittent load (fridge compressor cycling) averages to its real duty
    // cycle instead of swinging the estimate on every on/off.
    float    cavg_slow_alpha;    // 0.1/3600 — EWMA α at the 100 ms tick
} bame_config_t;


// Per-instance state. Self-contained; no pointers to external buffers.
typedef struct {
    // --- Configuration (set at init, rarely changed) ---
    uint8_t cells;
    bool    wiring_bus;

    // --- Capacity ---
    float capacity_ah;
    bool  capacity_learned;      // true once a cycle's amplitude exceeded nominal

    // --- SOC integrator (single source of truth) ---
    float coulomb_count;          // A·s
    bool  soc_uncertain;

    // --- Battery presence (set true on first valid tick; never goes false in
    // prod — if the BMS cuts off, BAME loses power and reboots) ---
    bool  battery_present;

    // --- Cycle bookkeeping ---
    float   coulombs_at_last_full;
    float   max_delivered_c_in_cycle;  // peak (coulombs_at_last_full - coulomb_count)
                                       // seen since last FULL; promoted to capacity
                                       // on the next FULL if it exceeds nominal
    uint32_t since_last_full_ms;

    // --- Event-detection internals ---
    uint32_t rest_at_top_since_ms;
    float    v_slow_avg;

    // --- Smoothing / auto-zero ---
    float current_offset;
    float c_avg;                  // EWMA-smoothed current (τ ≈ 30 s) — watts
    float c_avg_slow;             // EWMA-smoothed current (τ ≈ 1 h)  — autonomy
    uint32_t c_avg_slow_n;        // samples so far; drives the warm-up α = 1/n so
                                  // c_avg_slow is the true mean from the very first
                                  // tick and only becomes a 1 h EWMA after ~1 h
    bool  c_avg_init;

    // --- Last-tick derived values (mirrored for display / test harness) ---
    float voltage;                // last raw voltage read
    float current;                // last current after offset + dead-band
    bool  charging_external;      // LOAD-mode: charger detected, integration frozen
    bool  ext_charge_armed;       // LOAD-mode: allowed to enter charging_external state
                                  // (one-shot; consumed on entry, re-armed after a sustained
                                  //  below-top dip, NOT a brief post-disconnect rebond)
    uint32_t below_top_since_ms;  // millis when voltage first went below top (0 = not tracking)

    // --- BUS two-anchor capacity learning (aging-aware) ---
    // Capacity = EWMA of ΔAh/ΔSOC measured on the DISCHARGE leg only (FULL→KNEE
    // rest anchors); the charge leg would overestimate it (coulombic efficiency
    // < 1). Moves up AND down, so an aging pack is tracked.
    uint8_t  last_anchor_kind;       // 0 none, 1 full, 2 knee
    float    coulomb_at_last_anchor; // coulomb_count sampled at the last anchor
    float    soc_at_last_anchor;     // SOC% read from the OCV curve at that anchor
    uint32_t rest_at_knee_since_ms;  // sustained-rest timer for the KNEE anchor
    bool     full_armed;             // FULL anchor one-shot per top visit
    bool     knee_armed;             // KNEE anchor one-shot per bottom visit
} bame_state_t;


// Fill cfg with firmware default thresholds.
void bame_config_defaults(bame_config_t* cfg);

// Initialize state. `capacity_ah` is the starting (nominal) capacity;
// battery_present defaults to false until a voltage reading clears it.
void bame_init(bame_state_t* s, uint8_t cells, bool wiring_bus,
               float capacity_ah);

// One measurement tick. `voltage_raw` / `current_raw` are the sensor values
// BEFORE offset and dead-band (offset auto-zero is internal). `dt_s` is the
// seconds elapsed since the last call. `now_ms` is a monotonic millisecond
// timestamp (only used as a duration — wraparound-safe over runs < 49 days).
// Returns which event fired this tick, if any.
bame_event_t bame_step(bame_state_t* s, const bame_config_t* cfg,
                       float voltage_raw, float current_raw,
                       float dt_s, uint32_t now_ms);

// Declare the battery full (auto FULL event; no manual trigger in this build).
// Takes cfg so the amplitude-max learning step can clamp to cap_min/cap_max.
void bame_declare_full(bame_state_t* s, const bame_config_t* cfg,
                       uint32_t now_ms);

// Derived helpers (pure reads, no mutation).
float bame_capacity_as(const bame_state_t* s);
float bame_soc_percent(const bame_state_t* s);


#ifdef __cplusplus
}
#endif

#endif
