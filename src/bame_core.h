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
    BAME_EVT_NONE       = 0,
    BAME_EVT_FULL       = 1,   // battery full event (SOC → 100%)
    BAME_EVT_BMS_CUTOFF = 2,   // voltage collapsed, cycle closed
    BAME_EVT_PARTIAL    = 3,   // LOAD-mode unexplained V rise (soc_uncertain set)
} bame_event_t;


// Tunable thresholds. Filled by `bame_config_defaults()` to match firmware.
typedef struct {
    float    v_full_per_cell;   // 3.40  — rest voltage above this = at-top
    float    v_min_battery;     // 1.0   — below this = BMS cutoff
    float    i_rest;            // 0.3   — |I| below this = battery at rest
    uint32_t full_rest_ms;      // 30000 — sustained top+rest needed for full evt
    float    cavg_ewma_alpha;   // 0.1/30 — smoothed current constant
    float    cap_min_ah;        // 1.0   — sanity bounds
    float    cap_max_ah;        // 500.0
    float    v_rise_partial;    // 0.05  — LOAD mode: V rise > this = partial charge
} bame_config_t;


// Per-instance state. Self-contained; no pointers to external buffers.
typedef struct {
    // --- Configuration (set at init, rarely changed) ---
    uint8_t cells;
    bool    wiring_bus;

    // --- Capacity ---
    float capacity_ah;
    bool  capacity_learned;      // true once ≥1 full→cutoff cycle was measured

    // --- SOC integrator (single source of truth) ---
    float coulomb_count;          // A·s
    bool  soc_uncertain;

    // --- Battery presence ---
    bool  battery_present;

    // --- Cycle bookkeeping ---
    float   coulombs_at_last_full;
    uint32_t since_last_full_ms;

    // --- Event-detection internals ---
    uint32_t rest_at_top_since_ms;
    float    v_slow_avg;

    // --- Smoothing / auto-zero ---
    float current_offset;
    float c_avg;                  // EWMA-smoothed current
    bool  c_avg_init;

    // --- Last-tick derived values (mirrored for display / test harness) ---
    float voltage;                // last raw voltage read
    float current;                // last current after offset + dead-band
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

// Declare the battery full (manual menu action or external event).
void bame_declare_full(bame_state_t* s, uint32_t now_ms);

// Derived helpers (pure reads, no mutation).
float bame_capacity_as(const bame_state_t* s);
float bame_soc_percent(const bame_state_t* s);


#ifdef __cplusplus
}
#endif

#endif
