// SimulationEngine: replays one trace through many strategies and collects metrics.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cdn/Catalog.hpp"
#include "cdn/Json.hpp"
#include "cdn/Strategies.hpp"
#include "cdn/Traffic.hpp"

namespace cdn {

struct SimConfig {
    int numObjects = 5000;
    int epochs = 40;
    int epochLen = 15000;
    double capacityFrac = 0.05;  // cache size as a fraction of total catalog bytes
    std::uint64_t seed = 1;
    BenefitWeights weights;
    double hitLatencyMs = 8.0;   // edge RTT on a hit; a miss adds the object's origin latency
    int warmup = 2;              // epochs excluded from summary metrics
    TrafficConfig traffic = presetTraffic("zipf");
    std::vector<std::string> algos = defaultStrategyKeys();
    i64 maxUnits = 4096;
    std::string catalogCsv;
    std::string traceCsv;
    int threads = 0;             // 0 = hardware concurrency
};

struct EpochMetrics {
    i64 requests = 0;
    i64 hits = 0;
    double reqKB = 0, hitKB = 0;
    double latencySum = 0;
    double benefitAll = 0, benefitHit = 0;  // knapsack objective: sum of B over requests / hits
    double baseCost = 0;   // $ if nothing were cached
    double missCost = 0;   // $ paid on misses
    double prefetchKB = 0, prefetchCost = 0;
    double planMs = 0;
    int replans = 0;
    double usedFrac = 0;
    double accessMs = 0;
};

struct Summary {
    double hitRatio = 0, byteHitRatio = 0, benefitRatio = 0, avgLatencyMs = 0;
    double originGB = 0, prefetchGB = 0, requestedGB = 0, originOffloadPct = 0;
    double baseCost = 0, originCost = 0, costSavedPct = 0;
    double planMs = 0, nsPerRequest = 0;
    double replans = 0, driftReplans = 0, admissions = 0, evictions = 0;
    double maxUsedFrac = 0, plannerMemKB = 0;

    std::vector<std::pair<std::string, double>> fields() const;
};

struct AlgoRun {
    std::string key, name;
    bool oracle = false;
    std::vector<EpochMetrics> epochs;
    Summary summary;
    std::vector<int> finalContents;
    std::vector<PlanEvent> events;
};

struct SimRun {
    SimConfig cfg;
    i64 capacityKB = 0;
    std::shared_ptr<const Catalog> catalog;
    std::vector<Marker> markers;
    std::vector<AlgoRun> algos;
    double wallMs = 0;
    long long totalRequests = 0;
};

/// Run `fn(i)` for i in [0, n) on a small thread pool.
void parallelFor(int n, const std::function<void(int)>& fn, int threads = 0);

class SimulationEngine {
public:
    /// Build catalog + trace from cfg and run every algorithm in cfg.algos.
    static SimRun run(const SimConfig& cfg);
    /// Run a single strategy over a prepared trace.
    static AlgoRun runOne(ICacheStrategy& s, const Catalog& cat, const Trace& trace, const SimConfig& cfg);
    static Summary summarize(const std::vector<EpochMetrics>& ep, int warmup, const StrategyStats& st,
                             double maxUsedFrac);
};

/// Serialise a run. `detail` adds per-epoch series, final cache contents and plan events.
void writeRun(JsonWriter& j, const SimRun& run, bool detail);
void writeConfig(JsonWriter& j, const SimConfig& cfg);

}  // namespace cdn
