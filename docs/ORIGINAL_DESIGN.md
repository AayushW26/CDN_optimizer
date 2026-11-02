# CDN Cache Replacement Optimizer
## System Architecture & Technical Documentation

---

## Table of Contents

1. Introduction & Problem Statement
2. Goals & Scope
3. High-Level Architecture
4. Problem Formulation: DP / Knapsack Model
5. Baseline Comparison Algorithms (LRU, LFU, Greedy)
6. Traffic Pattern Generators
7. System Modules (C++ Design)
8. Data Structures Per Module
9. Metrics & Evaluation Methodology
10. Predictive Caching Extension
11. Dashboard Architecture (React)
12. Repository / Folder Structure
13. Build System & Tooling
14. File Formats / Interfaces
15. Complexity Summary Table
16. Core Pseudocode
17. Experimental Plan
18. Implementation Roadmap
19. Limitations & Future Work
20. Suggested Report Structure & References

---

## 1. Introduction & Problem Statement

A Content Delivery Network (CDN) edge server has finite cache storage `C` but serves requests for a much larger catalog of content objects (videos, images, files). Each object `i` has:

- **Size** `s_i` (storage cost)
- **Request frequency** `f_i` (popularity, requests per unit time)
- **Access/origin-fetch cost** `c_i` (bandwidth or dollar cost of a cache miss)
- **Network latency** `l_i` (latency penalty incurred on a miss)

**Goal:** choose a subset of objects to keep in cache such that total size ≤ `C`, while maximizing the aggregate benefit of caching (hit ratio, bandwidth saved, latency avoided), and re-evaluate this choice as traffic evolves over time.

This is structurally the **0/1 Knapsack Problem**: objects are items, size is weight, and a derived "caching value" is the item's value. The project's core contribution is:

1. Formulating cache selection as a knapsack DP and solving it optimally per time window.
2. Comparing this offline-optimal baseline against classic *online* heuristics (LRU, LFU, Greedy) under realistic, skewed, and bursty traffic.
3. Quantifying the accuracy/practicality trade-off: DP gives the *ceiling* on achievable performance for a given window of knowledge; LRU/LFU/Greedy show what's achievable with only causal, real-time information.

---

## 2. Goals & Scope

**In scope**
- C++ core simulation engine implementing DP-Knapsack, LRU, LFU, Greedy cache strategies
- Synthetic workload generators (Uniform, Zipf, Flash Crowd, Drifting Popularity)
- Metrics collection: hit ratio, byte hit ratio, bandwidth saved, average latency, memory usage, execution time
- JSON/CSV export of simulation results
- React dashboard for visualization and side-by-side algorithm comparison
- Optional: predictive pre-caching module

**Out of scope (explicitly, to keep the project bounded)**
- Real network I/O / actual CDN edge deployment
- Multi-tier cache hierarchies (edge + regional + origin) — mentioned only as future work
- Distributed/multi-node cache coordination

---

## 3. High-Level Architecture

The system is organized into five layers. C++ owns everything from workload generation through algorithm execution and metrics; the dashboard is a pure consumer of exported results (and optionally a live API).

```
┌──────────────────────────────────────────────────────────────────────┐
│                         PRESENTATION LAYER                           │
│   React Dashboard: cache grid, hit/miss timeline, algorithm compare  │
└───────────────────────────────▲──────────────────────────────────────┘
                                 │ REST/JSON (or static file read)
┌───────────────────────────────┴──────────────────────────────────────┐
│                          ANALYTICS LAYER                              │
│   MetricsCollector · ResultExporter (JSON/CSV) · Optional Node API    │
└───────────────────────────────▲──────────────────────────────────────┘
                                 │
┌───────────────────────────────┴──────────────────────────────────────┐
│                        ALGORITHM ENGINE LAYER                         │
│  ICacheStrategy interface implemented by:                             │
│   - KnapsackDPCache   - LRUCache   - LFUCache   - GreedyCache         │
│   - PredictiveCache (extension, wraps a base strategy)                │
└───────────────────────────────▲──────────────────────────────────────┘
                                 │
┌───────────────────────────────┴──────────────────────────────────────┐
│                         SIMULATION LAYER                              │
│   SimulationEngine (epoch loop) · RequestStream                       │
└───────────────────────────────▲──────────────────────────────────────┘
                                 │
┌───────────────────────────────┴──────────────────────────────────────┐
│                            DATA LAYER                                 │
│   TrafficGenerator: Uniform · Zipf · FlashCrowd · DriftingPopularity  │
│   CatalogLoader: synthetic or CSV-based content catalog               │
└──────────────────────────────────────────────────────────────────────┘
```

**Design principle:** every caching strategy implements the same `ICacheStrategy` interface (`admit`, `onAccess`, `evictIfNeeded`, `contents()`), so the `SimulationEngine` can run identical request streams through DP, LRU, LFU, and Greedy without duplicating simulation logic — this is what makes the comparison scientifically fair.

---

## 4. Problem Formulation: DP / Knapsack Model

### 4.1 Mapping Caching to 0/1 Knapsack

| Knapsack concept | CDN caching equivalent |
|---|---|
| Item `i` | Content object `i` |
| Weight `w_i` | Content size `s_i` (e.g., MB) |
| Value `v_i` | Caching benefit score `B_i` (defined below) |
| Capacity `W` | Cache capacity `C` |
| Selected subset | Set of objects kept in cache this epoch |

### 4.2 Benefit / Value Function

The value of caching object `i` should reflect *how much pain is avoided* by not fetching it from origin on every request:

```
B_i = f_i × (α·c_i + β·l_i)
```

- `f_i` — observed request frequency in the current epoch
- `c_i` — bandwidth/origin cost per fetch
- `l_i` — latency penalty per fetch
- `α, β` — tunable weights (let the dashboard expose sliders for these — good demo material)

This gives a *per-epoch* value, recomputed as frequency estimates update. Using `f_i` as a multiplier is what differentiates this from generic knapsack: it converts "worth caching once" into "worth caching given how often it will be requested this epoch."

### 4.3 DP Recurrence & Complexity

Standard 0/1 Knapsack DP, over `n` items and integer capacity `C` (sizes are discretized to integer units, e.g., MB):

```
dp[i][c] = max value achievable using first i items with capacity c

dp[0][c] = 0                                  for all c
dp[i][c] = dp[i-1][c]                         if s_i > c
dp[i][c] = max( dp[i-1][c],
                dp[i-1][c - s_i] + B_i )       otherwise
```

Answer: `dp[n][C]`. Reconstruct the selected set by walking back from `dp[n][C]`.

- **Time complexity:** `O(n · C)` — pseudo-polynomial (depends on the numeric capacity, not just item count)
- **Space complexity:** `O(n · C)` naive, reducible to `O(C)` with a 1D rolling array (see §16) — but then set reconstruction needs either a parallel "keep" table (`O(n·C)` bits) or item-by-item DP replay

### 4.4 Space Optimization

For large `C` (realistic edge caches can be tens of GB), `O(n·C)` is infeasible at fine granularity. Mitigations to implement and discuss in the report:

1. **Bucket the capacity** — discretize sizes to coarser units (e.g., 10 MB buckets) to shrink `C`.
2. **1D rolling DP** with a separate `bool keep[n][C]` bit-table only if reconstruction is needed, or accept O(n) replay of the whole DP to reconstruct (trades time for space).
3. **Meet-in-the-middle** for moderate `n` (< ~40) when `C` is huge — splits items into two halves, `O(2^(n/2))`.
4. **FPTAS (value-scaling)** for large `n` and large `C` — scale/round `B_i` to get a `(1-ε)`-approximate solution in `O(n³/ε)`, independent of `C`. This is worth implementing as the "practical DP" mode, since raw exact DP will not scale to production-sized catalogs.

### 4.5 Online Adaptation: Epoch-Based Re-Optimization

Real traffic is a stream, not a static set — so DP is re-run periodically, not once:

```
Divide the simulation timeline into epochs (e.g., every N requests or every T seconds).
For each epoch:
    1. Collect access counts f_i for all objects seen so far (sliding or full window)
    2. Recompute B_i for every candidate object
    3. Solve knapsack DP over the candidate set → optimal "ideal cache state" for this epoch
    4. Diff against current cache contents:
         - Evict objects not in the new optimal set
         - Fetch/admit objects newly in the optimal set (subject to a fetch-cost/churn penalty)
    5. Log metrics for the epoch
```

This epoch design is what lets DP be compared fairly against LRU/LFU (which update continuously) — you can vary epoch length as an experiment (see §17) to show the trade-off between DP's "lookahead accuracy" and its recomputation overhead/staleness.

### 4.6 Why This Is the "Offline-Optimal Upper Bound"

Within an epoch, DP sees the *actual* frequencies for that epoch (computed after the fact or from a short lookback window) and picks the truly optimal subset for that data. LRU/LFU/Greedy must decide *causally*, request by request, without hindsight. This is the central comparison the project demonstrates: **DP is the ceiling, online heuristics are the practical floor**, and the interesting question is how close the heuristics get.

---

## 5. Baseline Comparison Algorithms

### 5.1 LRU (Least Recently Used)
- **Structure:** hash map (`content_id → list iterator`) + doubly linked list ordered by recency
- **On access:** move item to front; if miss and cache full, evict tail (least recently used)
- **Complexity:** `O(1)` per access
- **Behavior:** good for recency-biased workloads; poor under Zipf when popular items are separated by many unrelated requests (they get evicted despite high long-term popularity)

### 5.2 LFU (Least Frequently Used)
- **Structure:** hash map (`content_id → frequency`) + min-heap or frequency-bucketed doubly linked lists (O(1) LFU variant)
- **On access:** increment frequency; if miss and cache full, evict minimum-frequency item
- **Complexity:** `O(log n)` with heap, `O(1)` amortized with frequency-bucket lists
- **Behavior:** captures long-term popularity well but reacts slowly to shifting trends (old popular items "stick" — cache pollution) — this is where **flash crowd** and **drifting popularity** traffic patterns are designed to expose weaknesses

### 5.3 Greedy (Value-Density Selection)
- **Structure:** compute `B_i / s_i` for all candidates, sort descending, admit greedily until capacity is exhausted
- **Complexity:** `O(n log n)` for the sort, dominates
- **Behavior:** the classic *fractional*-knapsack-style greedy is not optimal for 0/1 knapsack but is fast and a natural "why not just do this?" baseline against the DP-optimal solution — good for showing the DP's actual value-add in the report

### Comparison Table

| Algorithm | Decision basis | Time/access | Space | Optimality |
|---|---|---|---|---|
| DP Knapsack | Global, per-epoch optimal | O(n·C) per epoch | O(n·C) or O(C) | Optimal *for that epoch's data* |
| Greedy | Local value density | O(n log n) per epoch | O(n) | Approximate |
| LRU | Recency only | O(1) | O(n) | Heuristic |
| LFU | Frequency only | O(1)–O(log n) | O(n) | Heuristic |

---

## 6. Traffic Pattern Generators

| Pattern | Description | Generation method |
|---|---|---|
| **Uniform** | Every object equally likely to be requested | Uniform random draw over catalog |
| **Zipf** | Small fraction of objects get most requests (realistic web/video traffic) | Draw from Zipf distribution with skew parameter `s` (typically 0.7–1.2) |
| **Flash Crowd** | Sudden spike in requests for a small set of objects (viral content) | Inject a burst: for a random window, redirect X% of traffic to a small "hot set" |
| **Drifting Popularity** | Popular set changes over time (today's trending ≠ yesterday's) | Periodically reshuffle the Zipf rank-to-object mapping, or slide a popularity window |

Each generator should expose the same interface (`std::vector<Request> generateEpoch(int epochId)`), so the `SimulationEngine` treats them polymorphically. Skew/burst parameters should be CLI/config-driven for easy experimentation and dashboard sliders.

---

## 7. System Modules (C++ Design)

```
ICacheStrategy (abstract interface)
 ├── admit(item)
 ├── onAccess(id) -> bool (hit/miss)
 ├── evictIfNeeded(newItemSize)
 ├── contents() -> vector<CacheItem>
 └── name() -> string

Implementations:
 ├── KnapsackDPCache : ICacheStrategy
 ├── GreedyCache      : ICacheStrategy
 ├── LRUCache         : ICacheStrategy
 ├── LFUCache         : ICacheStrategy
 └── PredictiveCache  : decorates any ICacheStrategy (wraps + prefetches)

Supporting modules:
 ├── CacheItem            (id, size, freq, cost, latency, lastAccessTime)
 ├── Request               (id, timestamp)
 ├── TrafficGenerator (+ 4 subclasses: Uniform/Zipf/FlashCrowd/Drifting)
 ├── CatalogLoader          (synthetic or CSV-based catalog of objects)
 ├── SimulationEngine       (epoch loop; drives one strategy over one stream)
 ├── MetricsCollector       (hit ratio, byte hit ratio, bandwidth, latency, mem, time)
 ├── ResultExporter         (JSON/CSV writer for dashboard consumption)
 └── ExperimentRunner       (CLI entry point; runs all strategies × all patterns)
```

### Key class sketches

```cpp
struct CacheItem {
    int id;
    long sizeKB;
    double frequency;     // requests observed this epoch
    double fetchCost;     // bandwidth/origin cost per miss
    double latencyMs;     // latency penalty per miss
    long lastAccessTick;  // for LRU
};

class ICacheStrategy {
public:
    virtual bool onAccess(const CacheItem& item) = 0;   // returns true = hit
    virtual void beginEpoch(const std::vector<CacheItem>& candidates) = 0;
    virtual std::vector<CacheItem> contents() const = 0;
    virtual std::string name() const = 0;
    virtual ~ICacheStrategy() = default;
};

class KnapsackDPCache : public ICacheStrategy {
    long capacityKB;
    std::unordered_map<int, CacheItem> currentSet;
public:
    void beginEpoch(const std::vector<CacheItem>& candidates) override {
        auto selected = solveKnapsack(candidates, capacityKB); // DP core
        applyDiff(selected);   // evict/admit against currentSet
    }
    bool onAccess(const CacheItem& item) override {
        return currentSet.count(item.id) > 0;
    }
    // contents(), name() ...
};
```

This decorator pattern for `PredictiveCache` lets you wrap `KnapsackDPCache`, `LRUCache`, etc. with a forecasting pre-fetch step without duplicating eviction logic — implement the extension in §10 this way.

---

## 8. Data Structures Per Module

| Module | Core data structure | Why |
|---|---|---|
| KnapsackDPCache | 2D/1D DP table (`vector<vector<double>>` or rolled `vector<double>`) + keep-table for reconstruction | Needed for the DP recurrence itself |
| LRUCache | `unordered_map<id, list<CacheItem>::iterator>` + `list<CacheItem>` | O(1) move-to-front and eviction |
| LFUCache | `unordered_map<id, int freq>` + frequency-bucketed `list` per frequency (O(1) LFU) or `min-heap` | O(1) or O(log n) min-frequency eviction |
| GreedyCache | `vector<CacheItem>` sorted by `value/size` | Simple, sort-dominated |
| MetricsCollector | Running counters (`hits`, `misses`, `bytesServedFromCache`, `bytesFromOrigin`, epoch timers) | Aggregation across epochs |
| TrafficGenerator (Zipf) | Precomputed cumulative distribution array for inverse-transform sampling | Efficient repeated sampling |

---

## 9. Metrics & Evaluation Methodology

For every strategy × traffic pattern × cache-capacity combination, record per epoch and cumulatively:

- **Cache Hit Ratio** = hits / (hits + misses)
- **Byte Hit Ratio** = bytes served from cache / total bytes requested
- **Bandwidth Saved** = bytes served from cache × per-byte origin cost
- **Average Latency** = weighted average of (cache-hit latency vs. miss latency+fetch time)
- **Memory Usage** = actual bytes occupied vs. capacity (should stay ≤ C for correctness check)
- **Execution Time** = wall-clock time to run `beginEpoch`/`onAccess` — this is where DP's `O(n·C)` cost becomes visible and interesting vs. LRU/LFU's O(1)

Run each combination multiple times (different random seeds) and report mean ± std dev — this is what turns a demo into a proper DAA analysis.

---

## 10. Predictive Caching Extension

**Goal:** pre-fetch content likely to become popular *before* it's requested, rather than reacting after misses.

**Approach (kept intentionally simple/explainable for a DAA project):**

1. Maintain a rolling frequency history per object across the last `k` epochs.
2. Use exponential smoothing (or simple linear regression on the last `k` points) to forecast next-epoch frequency `f̂_i`.
3. Feed `f̂_i` (instead of last-epoch `f_i`) into the benefit function `B_i` *before* the epoch starts, and run the knapsack DP proactively.
4. Compare "predictive DP" vs. "reactive DP" hit ratio, especially during **flash crowd** and **drifting popularity** simulations — this is the strongest demo moment since predictive caching should visibly outperform reactive strategies right at the onset of a spike.

This can be implemented as a thin decorator (`PredictiveCache`) around `KnapsackDPCache`, overriding only the value computation step.

---

## 11. Dashboard Architecture (React)

### Data flow options (pick one based on how live you want it)

**Option A — Batch/offline (simplest, recommended for a course project):**
```
C++ SimulationEngine → writes results.json / results.csv per run
                     → React dashboard reads static JSON, renders charts
```

**Option B — Live (more impressive, more work):**
```
C++ core compiled as a library, wrapped by a small Node.js/Express bridge (via a
child process or FFI) → exposes REST endpoints (/simulate, /metrics, /cacheState)
→ React dashboard polls or uses WebSockets for live epoch-by-epoch updates
```

### Suggested components

| Component | Purpose |
|---|---|
| `ControlPanel` | Choose algorithm, traffic pattern, cache capacity, α/β weights, run button |
| `CacheGrid` | Visual grid of current cache contents, color-coded by recently hit/evicted |
| `HitMissTimeline` | Time series of hit ratio per epoch, one line per algorithm |
| `AlgorithmComparisonChart` | Bar/radar chart comparing hit ratio, bandwidth saved, latency, exec time across DP/LRU/LFU/Greedy |
| `EvictionLog` | Scrollable log of admit/evict decisions per epoch (great for explaining DP's reasoning in a viva/demo) |
| `MetricsSummaryCards` | Headline numbers: overall hit ratio, total bandwidth saved, avg latency |

Libraries: `recharts` or `chart.js` for charts, plain CSS/Tailwind for layout — no need for anything heavier.

---

## 12. Repository / Folder Structure

```
cdn-cache-optimizer/
├── core/                          # C++ engine
│   ├── include/
│   │   ├── ICacheStrategy.hpp
│   │   ├── CacheItem.hpp
│   │   ├── KnapsackDPCache.hpp
│   │   ├── LRUCache.hpp
│   │   ├── LFUCache.hpp
│   │   ├── GreedyCache.hpp
│   │   ├── PredictiveCache.hpp
│   │   ├── TrafficGenerator.hpp
│   │   ├── SimulationEngine.hpp
│   │   ├── MetricsCollector.hpp
│   │   └── ResultExporter.hpp
│   ├── src/
│   │   └── (matching .cpp files)
│   ├── tests/
│   │   └── (GoogleTest unit tests per module)
│   └── CMakeLists.txt
├── experiments/
│   ├── configs/                   # JSON/YAML experiment configs
│   └── run_all.sh                 # runs ExperimentRunner across configs
├── dashboard/                     # React app
│   ├── src/components/
│   ├── src/data/                  # reads exported JSON
│   └── package.json
├── results/                       # exported JSON/CSV per run
├── docs/
│   └── this document, plus diagrams
└── README.md
```

---

## 13. Build System & Tooling

- **Build:** CMake (C++17 or later)
- **Testing:** GoogleTest for unit tests on each `ICacheStrategy` implementation (correctness of hit/miss counting, capacity never exceeded, DP reconstruction matches `dp[n][C]` value)
- **Benchmarking:** `<chrono>` timers wrapped around `beginEpoch`/`onAccess`; optionally Google Benchmark for finer profiling of the DP table fill
- **Export:** a minimal JSON writer (either hand-rolled or `nlohmann/json` if external deps are acceptable) for `results/*.json`
- **Dashboard:** `create-react-app` or `Vite` + `recharts`

---

## 14. File Formats / Interfaces

### `results/<run_id>.json` schema (example)

```json
{
  "runId": "zipf_s1.0_cache512MB",
  "trafficPattern": "zipf",
  "cacheCapacityKB": 524288,
  "algorithms": {
    "DP_Knapsack": {
      "hitRatioByEpoch": [0.41, 0.55, 0.63, "..."],
      "bandwidthSavedMB": 812.4,
      "avgLatencyMs": 38.2,
      "execTimeMsByEpoch": [120.5, 118.9, "..."],
      "finalCacheContents": [{"id": 1042, "sizeKB": 2048}, "..."]
    },
    "LRU": { "...": "same shape" },
    "LFU": { "...": "same shape" },
    "Greedy": { "...": "same shape" }
  }
}
```

Keeping every algorithm's output in the same shape is what makes `AlgorithmComparisonChart` a simple map/reduce over this file rather than bespoke parsing per algorithm.

---

## 15. Complexity Summary Table

| Component | Time Complexity | Space Complexity |
|---|---|---|
| DP Knapsack (per epoch) | O(n·C) | O(n·C) naive / O(C) rolled |
| DP with FPTAS approximation | O(n³/ε) | O(n²/ε) |
| Greedy | O(n log n) | O(n) |
| LRU access | O(1) | O(n) |
| LFU access (bucketed) | O(1) amortized | O(n) |
| LFU access (heap-based) | O(log n) | O(n) |
| Zipf sampling (precomputed CDF) | O(log n) per sample (binary search) or O(1) with alias method | O(n) |

`n` = number of candidate objects considered per epoch, `C` = cache capacity (in discretized units), `ε` = FPTAS approximation parameter.

---

## 16. Core Pseudocode

### 16.1 Space-optimized DP with reconstruction support

```
function solveKnapsack(items[1..n], capacity C):
    dp = array of size (C+1), initialized to 0
    keep = 2D boolean array [1..n][0..C]   // needed only if reconstructing

    for i in 1..n:
        for c in C downto items[i].size:      // iterate downward: reuse row i-1 values
            candidate = dp[c - items[i].size] + items[i].benefit
            if candidate > dp[c]:
                dp[c] = candidate
                keep[i][c] = true

    // Reconstruction: walk backward using `keep`
    selected = []
    c = C
    for i in n downto 1:
        if keep[i][c]:
            selected.append(items[i])
            c = c - items[i].size

    return selected
```

### 16.2 Epoch-driven simulation loop

```
function runSimulation(strategy, trafficGenerator, numEpochs):
    for epochId in 1..numEpochs:
        requests   = trafficGenerator.generateEpoch(epochId)
        candidates = aggregateFrequencies(requests)   // build/update CacheItem list

        strategy.beginEpoch(candidates)  // DP re-optimizes; LRU/LFU/Greedy may no-op here

        for request in requests:
            hit = strategy.onAccess(request)
            metrics.record(epochId, hit, request)

        metrics.finalizeEpoch(epochId, strategy.contents())

    return metrics.summary()
```

---

## 17. Experimental Plan (for the report / viva)

Suggested experiment matrix — vary one axis at a time, hold others fixed:

1. **Cache capacity sweep** (e.g., 5%, 10%, 25%, 50% of total catalog size) → hit ratio curves per algorithm
2. **Zipf skew sweep** (s = 0.5 to 1.5) → shows DP/LFU advantage grows with skew
3. **Flash crowd intensity** → shows LRU/LFU lag vs. DP/Predictive reacting faster
4. **Epoch length sweep** → shows DP's staleness/overhead trade-off
5. **α/β weight sweep** in the benefit function → shows how "optimize for bandwidth" vs. "optimize for latency" changes the selected cache set
6. **Execution time vs. catalog size (n)** → empirically validate the O(n·C) claim, compare against Greedy/LRU/LFU's near-linear behavior

Each experiment should produce one comparison chart (already supported by the dashboard's `AlgorithmComparisonChart`).

---

## 18. Implementation Roadmap

| Phase | Deliverable |
|---|---|
| 1 | `CacheItem`, `ICacheStrategy`, brute-force + DP knapsack core, unit tests |
| 2 | LRU, LFU, Greedy implementations + correctness tests |
| 3 | Traffic generators (Uniform, Zipf) + basic `SimulationEngine` + `MetricsCollector` |
| 4 | Flash Crowd + Drifting Popularity generators; JSON `ResultExporter` |
| 5 | React dashboard (static JSON mode): control panel, timeline, comparison chart |
| 6 | Predictive caching extension + demo scenario (flash crowd + predictive vs. reactive) |
| 7 | Full experiment sweep (§17), write-up, polish dashboard (live mode optional) |

---

## 19. Limitations & Future Work

- DP re-optimization cost (`O(n·C)`) makes exact DP impractical at true production catalog/cache scale — the FPTAS/bucketing variants exist for this reason and should be discussed, not hidden.
- The benefit function `B_i` is a simplified linear combination; a real CDN would also weigh freshness/TTL, geographic request origin, and content correlation (e.g., video segments requested together).
- No multi-tier cache hierarchy (edge → regional → origin) is modeled — natural extension.
- No real network trace is used; results are on synthetic traffic. Using a public trace (e.g., a web-proxy or video-streaming access log) would strengthen the evaluation section significantly if you want to go further.

---

## 20. Suggested Report Structure & References

For the written report/viva, map directly onto the standard DAA project rubric:

1. Problem definition & motivation (§1)
2. Formal problem formulation as Knapsack (§4.1–4.2)
3. Algorithm design: DP + 3 baselines (§4–5)
4. Complexity analysis with proofs/derivation of O(n·C) (§4.3, §15)
5. Implementation overview (§7–8)
6. Experimental methodology & results (§9, §17)
7. Comparative analysis & discussion (why DP wins/loses where it does)
8. Conclusion & future work (§19)

**Topics worth reviewing while writing the analysis section:** 0/1 Knapsack DP proof of optimality, pseudo-polynomial time and NP-hardness of Knapsack, FPTAS via value scaling, competitive analysis of online algorithms (LRU/LFU competitive ratios are a classic topic and make for strong theoretical discussion alongside your empirical results).
