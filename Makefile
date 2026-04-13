# BaMe — common task wrapper around platformio + sim scripts.
#
# Override the default env in Makefile.local (gitignored), e.g.:
#     ENV = nano-load-4s
# See Makefile.local.example.

ENV ?= nano-bus-4s
-include Makefile.local

.DEFAULT_GOAL := build

# --- Firmware ---

build:
	pio run -e $(ENV)

upload:
	pio run -e $(ENV) -t upload

monitor:
	pio device monitor

clean:
	pio run -t clean

size:
	@pio run -e $(ENV) 2>&1 | grep -E "Flash|RAM" | head -2

list-envs:
	@grep -E "^\[env:" platformio.ini | sed 's/\[env://; s/\]//'

# --- Shared C core library (for sim/ via Python ctypes) ---
# Builds sim/bame_core.{dll,so,dylib} from src/bame_core.c so the Python
# sim calls the exact same code that runs on the AVR. Requires a host C
# compiler (gcc/clang/mingw).

# Pick extension per platform
ifeq ($(OS),Windows_NT)
  LIBEXT := dll
  LIBPREFIX :=
else
  UNAME_S := $(shell uname -s)
  ifeq ($(UNAME_S),Darwin)
    LIBEXT := dylib
  else
    LIBEXT := so
  endif
  LIBPREFIX := lib
endif
CORE_LIB := sim/$(LIBPREFIX)bame_core.$(LIBEXT)

CC ?= gcc

core-lib: $(CORE_LIB)

$(CORE_LIB): src/bame_core.c src/bame_core.h
	@command -v $(CC) >/dev/null 2>&1 || { \
	  echo "error: no C compiler found ($(CC)). Install mingw-w64 on Windows"; \
	  echo "       (winget install mingw), or gcc/clang on Linux/Mac."; \
	  exit 1; }
	$(CC) -shared -fPIC -O2 -Wall -Wextra src/bame_core.c -o $(CORE_LIB)
	@echo "built $(CORE_LIB)"

core-test: $(CORE_LIB)
	python sim/bame_core.py

# --- Sim & tools ---

sim-cal:
	python sim/calibration_sim.py --true-capacity 50 --nominal-capacity 80 --scenario deep

sim-trend:
	python sim/trend_sim.py --scenario glaciere --duration 600

screenshots:
	python sim/render_screens.py

.PHONY: build upload monitor clean size list-envs core-lib core-test sim-cal sim-trend screenshots
