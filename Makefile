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

# --- Sim & tools ---

sim-cal:
	python sim/calibration_sim.py --true-capacity 50 --nominal-capacity 80 --scenario deep

sim-trend:
	python sim/trend_sim.py --scenario glaciere --duration 600

screenshots:
	python sim/render_screens.py

.PHONY: build upload monitor clean size list-envs sim-cal sim-trend screenshots
