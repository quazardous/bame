"""ctypes wrapper around the compiled libbame_core (.so / .dll / .dylib).

Lets Python sim scripts call the exact same C code that runs on the AVR.
No more drift between firmware and sim.

Build the shared library first:
    make core-lib

Then use it:
    from bame_core import BameCore, EVT_FULL, EVT_BMS_CUTOFF
    core = BameCore(cells=4, wiring_bus=False, capacity_ah=80)
    evt = core.step(voltage=13.2, current_raw=3.7, dt_s=0.1, now_ms=100)
"""

import ctypes
import os
import sys
from ctypes import c_bool, c_float, c_uint8, c_uint32, c_int, POINTER, Structure


# --- Locate the shared library ---

def _lib_path():
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = ("bame_core.dll", "libbame_core.so", "libbame_core.dylib",
                  "bame_core.so")  # last one: non-standard but tolerated
    for name in candidates:
        p = os.path.join(here, name)
        if os.path.exists(p):
            return p
    raise OSError(
        "bame_core shared library not found in sim/.\n"
        "Run `make core-lib` from the project root to build it "
        "(requires gcc or clang). See QUICKSTART.md for help.")


# --- Struct layouts (must match bame_core.h) ---

class BameConfig(Structure):
    _fields_ = [
        ("v_full_per_cell",       c_float),
        ("v_min_battery",         c_float),
        ("i_rest",                c_float),
        ("full_rest_ms",          c_uint32),
        ("cavg_ewma_alpha",       c_float),
        ("cap_min_ah",            c_float),
        ("cap_max_ah",            c_float),
        ("v_rise_partial",        c_float),
        ("v_disconnect_drop",     c_float),
        ("ext_rearm_ms",          c_uint32),
    ]


class BameState(Structure):
    _fields_ = [
        ("cells",                 c_uint8),
        ("wiring_bus",            c_bool),
        ("capacity_ah",           c_float),
        ("capacity_learned",      c_bool),
        ("coulomb_count",         c_float),
        ("soc_uncertain",         c_bool),
        ("battery_present",       c_bool),
        ("coulombs_at_last_full", c_float),
        ("since_last_full_ms",    c_uint32),
        ("rest_at_top_since_ms",  c_uint32),
        ("v_slow_avg",            c_float),
        ("current_offset",        c_float),
        ("c_avg",                 c_float),
        ("c_avg_init",            c_bool),
        ("voltage",               c_float),
        ("current",               c_float),
        ("charging_external",     c_bool),
        ("ext_charge_armed",      c_bool),
        ("below_top_since_ms",    c_uint32),
    ]


# --- Event enum (matches bame_event_t) ---
EVT_NONE       = 0
EVT_FULL       = 1
EVT_BMS_CUTOFF = 2
EVT_PARTIAL    = 3


# --- Library loader ---

_lib = None

def _load():
    global _lib
    if _lib is not None:
        return _lib
    _lib = ctypes.CDLL(_lib_path())

    _lib.bame_config_defaults.restype = None
    _lib.bame_config_defaults.argtypes = [POINTER(BameConfig)]

    _lib.bame_init.restype = None
    _lib.bame_init.argtypes = [POINTER(BameState), c_uint8, c_bool, c_float]

    _lib.bame_step.restype = c_int
    _lib.bame_step.argtypes = [POINTER(BameState), POINTER(BameConfig),
                               c_float, c_float, c_float, c_uint32]

    _lib.bame_declare_full.restype = None
    _lib.bame_declare_full.argtypes = [POINTER(BameState), c_uint32]

    _lib.bame_soc_percent.restype = c_float
    _lib.bame_soc_percent.argtypes = [POINTER(BameState)]

    _lib.bame_capacity_as.restype = c_float
    _lib.bame_capacity_as.argtypes = [POINTER(BameState)]
    return _lib


# --- Friendly Python wrapper ---

class BameCore:
    def __init__(self, cells=4, wiring_bus=True, capacity_ah=80.0):
        lib = _load()
        self.state  = BameState()
        self.config = BameConfig()
        lib.bame_config_defaults(ctypes.byref(self.config))
        lib.bame_init(ctypes.byref(self.state), cells, wiring_bus, capacity_ah)

    def step(self, voltage, current_raw, dt_s, now_ms):
        return _lib.bame_step(ctypes.byref(self.state), ctypes.byref(self.config),
                              voltage, current_raw, dt_s, now_ms)

    def declare_full(self, now_ms):
        _lib.bame_declare_full(ctypes.byref(self.state), now_ms)

    @property
    def soc_percent(self):
        return _lib.bame_soc_percent(ctypes.byref(self.state))

    @property
    def capacity_ah(self):        return self.state.capacity_ah
    @property
    def coulomb_count(self):      return self.state.coulomb_count
    @property
    def current(self):            return self.state.current
    @property
    def voltage(self):            return self.state.voltage
    @property
    def c_avg(self):              return self.state.c_avg
    @property
    def capacity_learned(self):   return bool(self.state.capacity_learned)
    @property
    def soc_uncertain(self):      return bool(self.state.soc_uncertain)
    @property
    def battery_present(self):    return bool(self.state.battery_present)


if __name__ == "__main__":
    # Smoke test: init, step a few times, print state.
    c = BameCore(cells=4, wiring_bus=False, capacity_ah=80.0)
    print(f"init: capacity={c.capacity_ah}Ah, coulomb_count={c.coulomb_count:.0f} As")
    for t_ms in range(0, 1000, 100):
        evt = c.step(voltage=13.2, current_raw=3.0, dt_s=0.1, now_ms=t_ms)
        if evt != EVT_NONE:
            print(f"  t={t_ms}ms: event={evt}")
    print(f"after 1s @ 3A: coulomb_count={c.coulomb_count:.1f}, cAvg={c.c_avg:.3f} A")
    print(f"  (expected: ~{80*3600 - 3.0*1.0:.0f} As if battery_present transition worked)")
