# Architecture

This document describes the system as built. The original design brief is kept unchanged in
[`ORIGINAL_DESIGN.md`](ORIGINAL_DESIGN.md). The section [Deviations from the brief](#deviations-from-the-original-brief)
at the end lists what changed and why.

## Layers

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ PRESENTATION   dashboard/ (React 18 + Recharts, Vite)                          │
│   Overview · Timeline · Sweeps · Ablation · DAA lab · MCKP · DP visualizer ·   │
│   Live lab                                                                     │
└───────────────▲───────────────────────────────────────────▲──────────────────┘
                │ static JSON (results/*.json → public/data) │ POST /api/simulate
┌───────────────┴───────────────────────┐   ┌───────────────┴──────────────────┐
│ ANALYTICS   Simulation.cpp (metrics,  │   │ BRIDGE  server/server.mjs        │
│ JSON export) · Experiments.cpp        │   │ zero-dep Node; validates input,  │
│ (experiment matrix, paired stats)     │   │ execFile(cdnopt run --out -)     │
└───────────────▲───────────────────────┘   └──────────────────────────────────┘
┌───────────────┴──────────────────────────────────────────────────────────────┐
│ ALGORITHM ENGINE   Strategies.cpp — ICacheStrategy                            │
│   online: LRU · O(1) LFU · GDSF        oracles: Belady* · DP-Oracle*           │
│   planners: KnapsackPlannerCache {Greedy, DP-Reactive, DP-Predictive, PACK-DP} │
│   solvers: Knapsack.cpp (9 solvers + MCKP)                                     │
└───────────────▲──────────────────────────────────────────────────────────────┘
┌───────────────┴──────────────────────────────────────────────────────────────┐
│ SIMULATION   SimulationEngine::runOne: epoch loop, identical trace per strategy│
└───────────────▲──────────────────────────────────────────────────────────────┘
┌───────────────┴──────────────────────────────────────────────────────────────┐
│ DATA   Catalog (synthetic / CSV) · WorkloadGenerator (uniform, zipf, flash,     │
│        drift, mixed) · CSV trace loader · Random.hpp (xoshiro256**, alias)     │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Source map

| File | Responsibility |
|---|---|
| `core/include/cdn/Common.hpp` | `Object`, `Request`, `BenefitWeights`, `Stopwatch` |
| `core/include/cdn/Random.hpp` | xoshiro256** PRNG, Lemire bounded ints, Vose alias sampler (O(1) per sample) |
| `core/{include,src}/Catalog.*` | synthetic catalog (3 content kinds, 4 origin tiers), CSV catalog, benefit function |
| `core/{include,src}/Traffic.*` | workload model (Zipf base + head churn + content lifecycles + flash crowds + crawler scans), trace container with `nextUse`, CSV trace loader |
| `core/{include,src}/Knapsack.*` | 2-D DP, rolling DP + bitset, Hirschberg D&C DP, B&B (Martello-Toth U2), MITM, FPTAS, greedy, modified greedy, brute force, Dantzig bound, MCKP DP |
| `core/{include,src}/Strategies.*` | `ICacheStrategy`; `SizedLRU`; LRU, LFU, GDSF, Belady; `KnapsackPlannerCache` (+ Hedge forecaster, burst detector) |
| `core/{include,src}/Simulation.*` | `SimulationEngine`, metrics, summary, JSON export, `parallelFor` |
| `core/{include,src}/Experiments.*` | the experiment matrix (11 experiments) and aggregation |
| `core/apps/cdnopt.cpp` | CLI: `run`, `experiment`, `solve`, `list` |
| `core/tests/*` | dependency-free test framework + 20 test cases |
| `dashboard/src/*` | React dashboard |
| `server/server.mjs` | static server + simulation API |
| `scripts/make_figures.py` | report figures from `results/*.json` |

## The strategy interface

```cpp
class ICacheStrategy {
  virtual std::string name() const;
  virtual void bindTrace(const Trace*);          // oracles only (future knowledge)
  virtual void beginEpoch(int epoch);            // planners (re-)plan here
  virtual bool access(const Request&, i64 gi);   // serve; true = hit; admit/evict inside
  virtual void endEpoch(int epoch);              // planners update their forecasts
  virtual i64 usedKB() const;
  virtual std::vector<int> contents() const;
  StrategyStats stats;                           // prefetch bytes, admissions, plan time...
  std::vector<PlanEvent> events;                 // knapsack decisions for the plan log
};
```

The engine generates the whole trace once and replays it through every strategy. After each
access it checks `usedKB() ≤ C` (a capacity violation throws). The test suite also checks on every
access that a reported hit really was in `contents()`.

## KnapsackPlannerCache (all epoch planners, including PACK-DP)

```
beginEpoch(e):
    f  ← forecast()                      LastEpoch | EWMA | Holt | Hedge ensemble | Oracle
    replan(f)

replan(f):
    candidates  i with f_i > 0,  value v_i = f_i·B_i − γ·[i not cached]·admissionCost_i
    core prune  keep the densest items up to 4·C bytes   (Balas-Zemel / Pisinger core)
    solve       "auto": B&B on real byte sizes (node cap) → if not proven, also unit DP;
                keep the better plan after slack fill
    slack fill  add remaining items by density while they fit (exact bytes)
    diff        new items: promote from window (free) or prefetch (charged as origin bytes)
                dropped items: demote to window if room, else evict

access(r):
    hit = r ∈ planned ∨ r ∈ window
    miss → admit into window (if window capacity > 0: admission window or cold start)
    every epochLen/10 requests: burst detector (PACK only)

endEpoch(e):
    update forecaster state with this epoch's counts; Hedge re-weights its experts
```

Cold start: until a planner has seen one epoch, its whole cache works as an LRU. The first plan
promotes those objects for free.

### PACK-DP specifics

* **Forecaster:** a discounted-Hedge mixture of four experts (last epoch, EWMA α = 0.25,
  EWMA α = 0.6, damped Holt with φ = 0.8). Loss = benefit-weighted L1 forecast error, normalised.
  Weights are w_k ∝ exp(−η·L_k) with L_k ← 0.7·L_k + loss_k and η = 4.
* **Burst detector:** in each interval, object i is expected to receive μ_i = f̂_i · interval/epochLen
  requests. If k_i ≥ 4 and k_i > μ_i + 3√μ_i + 2, the object is an emerging heavy hitter. If the excess
  Σ(k_i − μ_i) is ≥ 2% of the interval's traffic, PACK re-plans with f̂_i ← max(f̂_i, observed rate),
  at most 3 times per epoch.
* **Churn penalty:** γ = 2 admission fetches.
* **No admission window:** it was evaluated (W-TinyLFU style, 10%) and hurt (see the ablation).

## Data formats

* `results/compare.json`, `ablation.json`: per pattern, for each algorithm the `mean`/`std` of every
  metric over seeds, plus `pairedVsPack` (per-seed paired deltas and win counts).
* `results/<sweep>.json` (capacity, skew, flash, epoch, weights, resolution): `x[]`, and per pattern
  and algorithm `mean.<metric>[]` / `std.<metric>[]` aligned with `x`.
* `results/timeline_<pattern>.json`: one detailed run, with per-epoch series, final cache contents and
  composition, the plan-event log, and flash/scan markers.
* `results/solvers.json`, `optgap.json`, `abr.json`: algorithm benchmarks (see the dashboard's DAA lab).

## Deviations from the original brief

| Brief | Built | Why |
|---|---|---|
| DP "sees the actual epoch frequencies" | split into **DP-Oracle*** (hindsight, labelled as an oracle) and causal planners (DP-Reactive / DP-Predictive / PACK-DP) | an algorithm that peeks at the epoch it is serving isn't deployable; mixing the two would overstate DP |
| prefetches free | prefetch bytes **charged as origin traffic** | otherwise planners get free bandwidth |
| hit ratio as headline | **benefit captured** (the knapsack objective) as headline, with all other metrics reported | it is what the algorithms optimise; hit ratio, byte hit ratio, $ and latency are all still shown |
| size bucketing for the DP | exact **B&B on real byte sizes** first; unit DP + slack fill as fallback | rounding alone lost up to ~9% of plan value (measured in `optgap`) |
| PredictiveCache = Holt decorator | kept as DP-Predictive; PACK-DP uses a **Hedge ensemble** | a single Holt model was beaten by plain EWMA on stationary traffic; the ensemble adapts |
| GoogleTest | built-in tiny test framework | hermetic build, no network needed |
| — | **added:** GDSF and Belady* baselines, Hirschberg-space DP, MITM, MT-U2 B&B, MCKP for multi-bitrate video, burst detector, paired statistics, Live lab, DP visualizer, CSV trace replay | depth for the DAA analysis and realism |
