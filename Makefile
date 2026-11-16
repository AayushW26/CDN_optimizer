# One-stop commands. Requires: CMake >= 3.16, a C++17 compiler, Node >= 18 (dashboard), Python 3 + matplotlib (figures).
BUILD := core/build
BIN   := $(BUILD)/cdnopt

.PHONY: all build test quick experiments figures dashboard serve clean lab lab-test traces heldout lab-figures

all: build test experiments figures dashboard

build:
	cmake -S core -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j

test: build
	$(BUILD)/cdn_tests

quick: build            ## ~30 s smoke run of every experiment (smaller matrix)
	$(BIN) experiment all --quick --out-dir results

experiments: build      ## full experiment matrix used in the report (~5 min on 4 cores)
	$(BIN) experiment all --out-dir results

figures:                ## PNG figures for README / report
	python3 scripts/make_figures.py

dashboard:
	cd dashboard && npm ci && npm run build

serve: build dashboard  ## dashboard + live simulation API on http://localhost:8787
	node server/server.mjs

clean:
	rm -rf $(BUILD) dashboard/dist dashboard/node_modules

# ---------------------------------------------------------------- cache lab (real traces, SHADE)
TRACES ?= traces
lab:
	cmake -S lab -B lab/build -DCMAKE_BUILD_TYPE=Release
	cmake --build lab/build -j

lab-test: lab
	lab/build/cachelab_tests

traces: lab             ## download + convert the 3 public traces (~3 GB) into $(TRACES)
	lab/experiments/get_traces.sh $(TRACES)

heldout: lab            ## full held-out evaluation -> results/lab (~1.5 h on 4 cores)
	lab/experiments/run_heldout.sh $(TRACES) results/lab

lab-figures:
	python3 scripts/lab_figures.py
