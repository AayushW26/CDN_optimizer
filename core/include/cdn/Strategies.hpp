// Cache replacement strategies. Every strategy implements ICacheStrategy, so
// the SimulationEngine replays an identical request stream through all of them.
//
//  Online (causal)            Epoch planners (knapsack)          Oracles (hindsight)
//  -----------------------    --------------------------------   -------------------
//  LRU   O(1)                 Greedy-Epoch   density greedy       DP-Oracle*  exact DP on the
//  LFU   O(1) freq-buckets    DP-Reactive    DP on last epoch                 epoch's true counts
//  GDSF  O(log n)             DP-Predictive  DP on Holt forecast  Belady*     furthest-next-use
//                             PACK-DP        (this project)
#pragma once

#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cdn/Catalog.hpp"
#include "cdn/Common.hpp"
#include "cdn/Traffic.hpp"

namespace cdn {

struct StrategyStats {
    i64 prefetchKB = 0;       // bytes pulled from origin proactively (count as origin traffic)
    double prefetchCost = 0;  // $ of those pulls
    i64 admissions = 0;
    i64 evictions = 0;
    int replans = 0;          // knapsack re-optimisations (epoch + drift triggered)
    int driftReplans = 0;
    double planMs = 0;        // total time spent inside the knapsack solver
    std::size_t plannerMemBytes = 0;  // peak solver table memory
};

/// One knapsack (re-)planning decision, exported for the dashboard's eviction log.
struct PlanEvent {
    int epoch = 0;
    i64 atRequest = 0;
    std::string reason;  // epoch | drift
    std::string solver;
    int candidates = 0;
    double solveMs = 0;
    double planValue = 0;
    double expectedHitRatio = 0;
    i64 plannedKB = 0;
    std::vector<int> admitted;
    std::vector<int> evicted;
};

class ICacheStrategy {
public:
    ICacheStrategy(const Catalog& cat, i64 capacityKB, BenefitWeights w)
        : cat_(cat), cap_(capacityKB), w_(w) {}
    virtual ~ICacheStrategy() = default;

    virtual std::string name() const = 0;
    /// Oracles get access to the full trace; causal strategies ignore it.
    virtual void bindTrace(const Trace* /*trace*/) {}
    virtual void beginEpoch(int /*epoch*/) {}
    /// Serve one request; returns true on a cache hit. Admission/eviction inside.
    virtual bool access(const Request& r, i64 globalIndex) = 0;
    virtual void endEpoch(int /*epoch*/) {}
    virtual i64 usedKB() const = 0;
    virtual std::vector<int> contents() const = 0;
    virtual bool isOracle() const { return false; }

    StrategyStats stats;
    std::vector<PlanEvent> events;
    i64 capacityKB() const { return cap_; }

protected:
    const Catalog& cat_;
    i64 cap_;
    BenefitWeights w_;
};

/// Byte-capacity LRU list, reused by LRUCache and by the planners' admission window.
class SizedLRU {
public:
    explicit SizedLRU(i64 capacityKB = 0) : cap_(capacityKB) {}
    bool contains(int id) const { return pos_.count(id) > 0; }
    bool touch(int id);  // move to MRU if present
    /// Insert as MRU, evicting LRU entries to make room. Returns false if it can never fit.
    bool insert(int id, i64 sizeKB, std::vector<int>* evicted = nullptr);
    bool remove(int id);
    void setCapacity(i64 capKB, std::vector<int>* evicted = nullptr);
    i64 used() const { return used_; }
    i64 capacity() const { return cap_; }
    std::vector<int> ids() const { return {order_.begin(), order_.end()}; }
    /// Insert at the LRU end only if there is free room (used for demotions).
    bool insertColdIfRoom(int id, i64 sizeKB);

private:
    i64 cap_;
    i64 used_ = 0;
    std::list<int> order_;  // front = most recently used
    std::unordered_map<int, std::pair<std::list<int>::iterator, i64>> pos_;
};

// ---------------------------------------------------------------- online --
class LRUCache final : public ICacheStrategy {
public:
    LRUCache(const Catalog& c, i64 cap, BenefitWeights w) : ICacheStrategy(c, cap, w), lru_(cap) {}
    std::string name() const override { return "LRU"; }
    bool access(const Request& r, i64 gi) override;
    i64 usedKB() const override { return lru_.used(); }
    std::vector<int> contents() const override { return lru_.ids(); }

private:
    SizedLRU lru_;
};

/// O(1) LFU (Shah, Mitra & Matani 2010): a list of frequency nodes, each holding
/// an LRU-ordered list of objects. Ties broken by recency.
class LFUCache final : public ICacheStrategy {
public:
    LFUCache(const Catalog& c, i64 cap, BenefitWeights w) : ICacheStrategy(c, cap, w) {}
    std::string name() const override { return "LFU"; }
    bool access(const Request& r, i64 gi) override;
    i64 usedKB() const override { return used_; }
    std::vector<int> contents() const override;

private:
    struct FreqNode {
        i64 freq;
        std::list<int> items;  // front = most recent
    };
    struct Entry {
        std::list<FreqNode>::iterator node;
        std::list<int>::iterator it;
    };
    void evictOne();
    std::list<FreqNode> freqs_;  // ascending frequency
    std::unordered_map<int, Entry> map_;
    i64 used_ = 0;
};

/// Greedy-Dual-Size-Frequency (Cherkasova 1998) -- the policy used by Squid.
/// H(i) = L + freq(i) * benefit(i) / size(i); evict min H; L inflates to the evicted H.
class GDSFCache final : public ICacheStrategy {
public:
    GDSFCache(const Catalog& c, i64 cap, BenefitWeights w) : ICacheStrategy(c, cap, w) {}
    std::string name() const override { return "GDSF"; }
    bool access(const Request& r, i64 gi) override;
    i64 usedKB() const override { return used_; }
    std::vector<int> contents() const override;

private:
    struct Meta {
        double h;
        i64 freq;
    };
    double L_ = 0;
    i64 used_ = 0;
    std::set<std::pair<double, int>> heap_;
    std::unordered_map<int, Meta> meta_;
};

// --------------------------------------------------------------- oracles --
/// Size-aware Belady (furthest next use) with bypass. Optimal for unit sizes;
/// a strong (not exact) reference bound for variable sizes. Needs the future.
class BeladyCache final : public ICacheStrategy {
public:
    BeladyCache(const Catalog& c, i64 cap, BenefitWeights w) : ICacheStrategy(c, cap, w) {}
    std::string name() const override { return "Belady*"; }
    void bindTrace(const Trace* t) override { trace_ = t; }
    bool access(const Request& r, i64 gi) override;
    i64 usedKB() const override { return used_; }
    std::vector<int> contents() const override;
    bool isOracle() const override { return true; }

private:
    const Trace* trace_ = nullptr;
    i64 used_ = 0;
    std::set<std::pair<i64, int>> byNext_;  // (next use, id)
    std::unordered_map<int, i64> next_;
};

// -------------------------------------------------------------- planners --
/// How a planner estimates next-epoch request frequency f_i.
///   LastEpoch : f = last epoch's count                     (reactive)
///   Ewma      : exponentially weighted moving average
///   Holt      : Holt's linear trend (level + trend)        ("predictive caching", doc s10)
///   Ensemble  : discounted-Hedge mixture of 4 experts       (PACK)
///   Oracle    : the epoch's true counts (hindsight)        (upper reference)
enum class FreqModel { LastEpoch, Ewma, Holt, Ensemble, Oracle };

struct PlannerConfig {
    std::string name = "DP";
    FreqModel model = FreqModel::LastEpoch;
    /// "auto"   : exact B&B on real byte sizes, falls back to unit DP (rolling) if B&B hits its node cap
    /// "rolling"/"hirschberg"/"dp2d": DP on sizes discretised to capacity/maxUnits
    /// "greedy" / "fptas:<eps>" / "bnb": run on real byte sizes
    std::string solver = "auto";
    double churnPenalty = 0.0;    // gamma: newly admitted objects are charged gamma x fetch cost
    bool driftDetect = false;     // burst detector -> mid-epoch re-planning
    double windowFrac = 0.0;      // share of capacity for an LRU admission window
    i64 maxUnits = 4096;          // DP capacity resolution (C in O(nC))
    double corePruneFactor = 4.0; // keep densest candidates up to k x capacity (0 = off)
    long long bnbNodeLimit = 400000;
    double ewmaAlpha = 0.5;
    double holtAlpha = 0.6;
    double holtBeta = 0.4;
    // Hedge ensemble
    double hedgeEta = 4.0;        // learning rate on normalised loss
    double hedgeDiscount = 0.7;   // forget old losses (tracks regime changes)
    // burst detector (Poisson z-test for emerging heavy hitters)
    int checksPerEpoch = 10;
    int maxDriftReplans = 3;      // per epoch
    double burstZ = 3.0;          // z-score threshold per object
    int burstMinCount = 4;        // ignore objects with fewer requests in the interval
    double burstMass = 0.02;      // trigger when anomalous excess >= 2% of interval traffic
};

/// Epoch-based knapsack planner. The cache is split into a *planned* region
/// (the knapsack solution, prefetched at plan time) and an optional LRU
/// *window* that absorbs objects the plan did not anticipate.
class KnapsackPlannerCache final : public ICacheStrategy {
public:
    static constexpr int kExperts = 4;  // last, slow EWMA, fast EWMA, damped Holt

    KnapsackPlannerCache(const Catalog& c, i64 cap, BenefitWeights w, PlannerConfig cfg, int epochLen);
    std::string name() const override { return cfg_.name; }
    void bindTrace(const Trace* t) override { trace_ = t; }
    void beginEpoch(int epoch) override;
    bool access(const Request& r, i64 gi) override;
    void endEpoch(int epoch) override;
    i64 usedKB() const override { return plannedKB_ + window_.used(); }
    std::vector<int> contents() const override;
    bool isOracle() const override { return cfg_.model == FreqModel::Oracle; }

    const PlannerConfig& config() const { return cfg_; }
    /// Current Hedge weights of the forecasting experts (Ensemble model only).
    std::vector<double> expertWeights() const { return {hedgeW_, hedgeW_ + kExperts}; }
    static const char* expertName(int k);

private:
    void replan(const std::vector<double>& freq, const std::string& reason, i64 gi);
    std::vector<double> forecast() const;
    double expertForecast(int k, std::size_t id) const;
    void checkBurst(i64 gi);

    PlannerConfig cfg_;
    int epochLen_;
    const Trace* trace_ = nullptr;
    int epoch_ = 0;
    i64 reqInEpoch_ = 0;

    std::unordered_set<int> planned_;
    i64 plannedKB_ = 0;
    SizedLRU window_;

    // causal statistics
    std::vector<double> count_;       // requests this epoch
    std::vector<int> touched_;        // ids with count_ > 0
    std::vector<double> recent_;      // requests since the last burst check
    std::vector<int> recentTouched_;
    std::vector<double> level_, trend_;  // EWMA / Holt state (single-model planners)
    std::vector<double> ex_[kExperts];   // expert state: last, ewmaSlow, ewmaFast, holtLevel
    std::vector<double> exTrend_;        // damped-Holt trend
    double hedgeLoss_[kExperts] = {0, 0, 0, 0};
    double hedgeW_[kExperts] = {0.25, 0.25, 0.25, 0.25};
    std::vector<char> tracked_;
    std::vector<int> trackedIds_;
    bool haveHistory_ = false;

    // burst detector state
    std::vector<double> fcur_;           // forecast in force this epoch (incl. boosts)
    i64 intervalReq_ = 0;
    int driftReplansThisEpoch_ = 0;
};

/// Factory: lru | lfu | gdsf | belady | greedy-epoch | dp-reactive | dp-units |
/// dp-ewma | dp-predictive | dp-oracle | pack | pack-noforecast | pack-nochurn |
/// pack-nodrift | pack-window | pack-greedy | pack-fptas
std::unique_ptr<ICacheStrategy> makeStrategy(const std::string& key, const Catalog& cat, i64 capacityKB,
                                             BenefitWeights w, int epochLen, i64 maxUnits = 4096);
std::vector<std::string> allStrategyKeys();
std::vector<std::string> defaultStrategyKeys();

}  // namespace cdn
