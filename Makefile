# Dev convenience targets. Set PI=user@host before remote targets.
#
#   make validate              YAML schema check, runs locally
#   make bootstrap PI=pi@host  one-time Pi setup (apt + esphome + groups)
#   make sync PI=pi@host       rsync the repo to the Pi
#   make compile PI=pi@host    sync + esphome compile on the Pi
#   make run PI=pi@host        sync + esphome run on the Pi (interactive)
#   make logs PI=pi@host       tail logs without re-running
#   make info PI=pi@host       print kernel / gpiochips / lgpio / model
#   make clean PI=pi@host      wipe the Pi's .esphome and .pio caches

PI ?=
HOST_DIR := esphome-host-linux
EXAMPLE ?= examples/01-gpio-button-led.yaml
RSYNC_EXCLUDES := --exclude .git --exclude .esphome --exclude .pio \
                  --exclude __pycache__ --exclude .DS_Store --exclude build

.PHONY: help validate sync bootstrap compile run logs info clean _require-pi

help:
	@grep -E '^[a-zA-Z_-]+:.*?##' $(MAKEFILE_LIST) | \
	  awk 'BEGIN{FS=":.*?##"};{printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'

validate: ## Validate YAML schema locally (no compile, no Pi needed)
	esphome config $(EXAMPLE)

sync: _require-pi ## Rsync repo to the Pi
	rsync -az --delete $(RSYNC_EXCLUDES) ./ $(PI):$(HOST_DIR)/

bootstrap: _require-pi ## One-time Pi setup (deps + esphome venv)
	scp scripts/pi-bootstrap.sh $(PI):/tmp/
	ssh -t $(PI) "bash /tmp/pi-bootstrap.sh"

compile: sync ## Compile on the Pi without running
	ssh -t $(PI) "cd $(HOST_DIR) && ~/esphome-venv/bin/esphome compile $(EXAMPLE)"

run: sync ## Sync, compile, and run the binary on the Pi
	ssh -t $(PI) "cd $(HOST_DIR) && ~/esphome-venv/bin/esphome run $(EXAMPLE) --device localhost"

logs: _require-pi ## Tail logs from a running instance
	ssh -t $(PI) "cd $(HOST_DIR) && ~/esphome-venv/bin/esphome logs $(EXAMPLE)"

info: _require-pi ## Print Pi GPIO + system info for debugging
	ssh $(PI) 'bash -s' < scripts/pi-info.sh

clean: _require-pi ## Wipe build caches on the Pi
	ssh $(PI) "rm -rf $(HOST_DIR)/.esphome $(HOST_DIR)/.pio"

_require-pi:
	@test -n "$(PI)" || { echo "Set PI=user@host (e.g. make sync PI=pi@raspberrypi.local)"; exit 1; }
