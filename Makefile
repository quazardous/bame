# BaMe — common task wrapper around platformio + sim scripts.
#
# Override the default env in Makefile.local (gitignored), e.g.:
#     ENV = nano-load-4s
# See Makefile.local.example.

ENV ?= nano-bus-4s
-include Makefile.local

.DEFAULT_GOAL := help

ifeq ($(OS),Windows_NT)
HELP_CMD = powershell -NoProfile -Command "Write-Host 'Usage: make <target>'; Write-Host ''; Write-Host 'Targets:'; Select-String -Path Makefile -Pattern '^[a-zA-Z_-]+:.*##' | ForEach-Object { $$p = $$_.Line -split ':[^#]*##\s*', 2; '  {0,-15} {1}' -f $$p[0], $$p[1] }"
else
HELP_CMD = echo "Usage: make <target>"; echo ""; echo "Targets:"; grep -E '^[a-zA-Z_-]+:.*##' Makefile | sed 's/:[^#]*##[ 	]*/	/'
endif

help:  ## Show this help
	@$(HELP_CMD)

# --- Firmware ---

build:  ## Build firmware for $(ENV)
	pio run -e $(ENV)

upload:  ## Flash firmware to device
	pio run -e $(ENV) -t upload

monitor:  ## Open serial monitor
	pio device monitor

clean:  ## Clean PlatformIO build artifacts
	pio run -t clean

size:  ## Show Flash/RAM usage for $(ENV)
	@pio run -e $(ENV) 2>&1 | grep -E "Flash|RAM" | head -2

list-envs:  ## List PlatformIO environments
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

core-lib: $(CORE_LIB)  ## Build shared C core lib for sim/

$(CORE_LIB): src/bame_core.c src/bame_core.h
	@command -v $(CC) >/dev/null 2>&1 || { \
	  echo "error: no C compiler found ($(CC)). Install mingw-w64 on Windows"; \
	  echo "       (winget install mingw), or gcc/clang on Linux/Mac."; \
	  exit 1; }
	$(CC) -shared -fPIC -O2 -Wall -Wextra src/bame_core.c -o $(CORE_LIB)
	@echo "built $(CORE_LIB)"

core-test: $(CORE_LIB)  ## Run Python smoke test against core lib
	python sim/bame_core.py

# --- Sim & tools ---

sim-cal:  ## Run calibration sim (3 cycles, 50/80 Ah)
	python sim/calibration_sim.py --true-capacity 50 --nominal-capacity 80 --cycles 3

sim-opt:  ## Optimize core thresholds (GA over the real C core)
	python sim/optimize.py

screenshots:  ## Render UI screen mockups
	python sim/render_screens.py

.PHONY: help build upload monitor clean size list-envs core-lib core-test sim-cal sim-opt screenshots
