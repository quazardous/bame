# BaMe — common task wrapper around platformio + sim scripts.
#
# Override the default env in Makefile.local (gitignored), e.g.:
#     ENV = nano-load-4s
# See Makefile.local.example.

ENV ?= nano-bus-4s
-include Makefile.local

.DEFAULT_GOAL := help

ifeq ($(OS),Windows_NT)
HELP_CMD = powershell -NoProfile -Command "Write-Host 'Usage: make <target>'; Write-Host ''; Write-Host 'Targets:'; Select-String -Path Makefile -Pattern '^[a-zA-Z_-]+:.*\#\#' | ForEach-Object { $$p = $$_.Line -split ':[^\#]*\#\#\s*', 2; '  {0,-15} {1}' -f $$p[0], $$p[1] }"
else
HELP_CMD = echo "Usage: make <target>"; echo ""; echo "Targets:"; grep -E '^[a-zA-Z_-]+:.*\#\#' Makefile | sed 's/:[^\#]*\#\#[ 	]*/	/'
endif

help:  ## Show this help
	@$(HELP_CMD)

# --- Host setup ---------------------------------------------------------
# One-shot dev-env install for Linux/macOS — the counterpart to setup.ps1.
# Installs the C compiler, avrdude (ISP flashing), PlatformIO and Pillow.
# PlatformIO goes through pipx (isolated venv) so it sidesteps PEP 668 AND
# repairs a broken pip install: `pipx install --force` rebuilds it clean.

setup:  ## Install/repair host toolchain (gcc, avrdude, PlatformIO, Pillow)
	@set -e; \
	echo ">> BaMe host setup"; \
	if   command -v dnf     >/dev/null 2>&1; then PKG="sudo dnf install -y";       SYS="gcc avrdude python3 python3-pip pipx python3-pillow"; \
	elif command -v apt-get >/dev/null 2>&1; then PKG="sudo apt-get install -y";   SYS="build-essential avrdude python3 python3-pip pipx python3-pil"; \
	elif command -v pacman  >/dev/null 2>&1; then PKG="sudo pacman -S --noconfirm";SYS="gcc avrdude python python-pipx python-pillow"; \
	elif command -v zypper  >/dev/null 2>&1; then PKG="sudo zypper install -y";    SYS="gcc avrdude python3 python3-pipx python3-Pillow"; \
	elif command -v brew    >/dev/null 2>&1; then PKG="brew install";              SYS="avrdude pipx"; \
	else echo "!! no supported package manager (dnf/apt/pacman/zypper/brew)."; \
	     echo "   install by hand: gcc, avrdude, pipx, python3 Pillow."; exit 1; fi; \
	echo ">> system packages: $$SYS"; \
	$$PKG $$SYS; \
	echo ">> PlatformIO via pipx (isolated; --force also repairs a broken install)"; \
	pipx install --force platformio; \
	pipx ensurepath >/dev/null 2>&1 || true; \
	if ! python3 -c "import PIL" >/dev/null 2>&1; then \
	  echo ">> Pillow not found from system pkg — trying pip --user"; \
	  python3 -m pip install --user Pillow || echo "!! install Pillow manually"; \
	fi; \
	echo ">> done. Open a fresh shell so PATH picks up pipx, then: make check"

check:  ## Verify the host toolchain is present
	@ok=1; \
	if command -v gcc >/dev/null 2>&1; then printf "  ok    %-9s %s\n" gcc "$$(gcc --version | head -1)"; \
	  else printf "  MISS  %-9s (run: make setup)\n" gcc; ok=0; fi; \
	if command -v avrdude >/dev/null 2>&1; then printf "  ok    %-9s %s\n" avrdude "$$(avrdude '-?' 2>&1 | grep -io 'version [0-9][^ ,]*' | head -1)"; \
	  else printf "  MISS  %-9s (run: make setup)\n" avrdude; ok=0; fi; \
	if command -v python3 >/dev/null 2>&1; then printf "  ok    %-9s %s\n" python3 "$$(python3 --version 2>&1)"; \
	  else printf "  MISS  %-9s (run: make setup)\n" python3; ok=0; fi; \
	if pio --version >/dev/null 2>&1; then printf "  ok    %-9s %s\n" pio "$$(pio --version 2>&1)"; \
	  else printf "  MISS  %-9s (absent or broken — run: make setup)\n" pio; ok=0; fi; \
	if python3 -c "import PIL" >/dev/null 2>&1; then printf "  ok    %-9s %s\n" Pillow "$$(python3 -c 'import PIL; print(PIL.__version__)')"; \
	  else printf "  MISS  %-9s (run: make setup)\n" Pillow; ok=0; fi; \
	if [ $$ok = 1 ]; then echo ">> toolchain OK"; else echo ">> issues above — run: make setup"; exit 1; fi

setup-udev:  ## Install PlatformIO udev rules (flash without sudo — Linux)
	@echo ">> installing PlatformIO udev rules (grants USBasp/serial access to your login)"; \
	curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
	  | sudo tee /etc/udev/rules.d/99-platformio-udev.rules >/dev/null; \
	sudo udevadm control --reload-rules; sudo udevadm trigger; \
	echo ">> done — replug the USBasp (re-login if a group was added)"

# --- Firmware ---

build:  ## Build firmware for $(ENV)
	pio run -e $(ENV)

upload:  ## Flash firmware to device
	pio run -e $(ENV) -t upload

monitor:  ## Open serial monitor
	pio device monitor

clean:  ## Clean PlatformIO build artifacts
	pio run -t clean

distclean:  ## Deep clean: PlatformIO cache + sim shared libs
	rm -rf .pio
	rm -f sim/bame_core.dll sim/libbame_core.so sim/libbame_core.dylib sim/bame_core.so

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

.PHONY: help setup check setup-udev build upload monitor clean distclean size list-envs core-lib core-test sim-cal sim-opt screenshots
