#include <algorithm>
#include <set>
#include <unordered_set>

#include "TestFramework.hpp"
#include "cdn/Simulation.hpp"
#include "cdn/Strategies.hpp"

using namespace cdn;

namespace {

Catalog unitCatalog(int n) { return Catalog::fromSizes(std::vector<i64>(static_cast<std::size_t>(n), 1)); }

Trace manualTrace(const std::vector<int>& ids, std::size_t numObjects, int epochLen) {
    Trace t;
    for (int id : ids) t.requests.push_back({id});
    for (std::size_t s = 0; s < ids.size(); s += static_cast<std::size_t>(epochLen)) t.epochStart.push_back(s);
    t.epochStart.push_back(ids.size());
    t.computeNextUse(numObjects);
    return t;
}

}  // namespace

TEST(lru_evicts_least_recently_used) {
    Catalog cat = unitCatalog(10);
    LRUCache c(cat, 2, {});
    CHECK(!c.access({0}, 0));
    CHECK(!c.access({1}, 1));
    CHECK(c.access({0}, 2));   // 0 becomes MRU
    CHECK(!c.access({2}, 3));  // evicts 1
    CHECK(c.access({0}, 4));
    CHECK(!c.access({1}, 5));
    CHECK(c.usedKB() == 2);
}

TEST(lfu_evicts_least_frequently_used_with_lru_tiebreak) {
    Catalog cat = unitCatalog(10);
    LFUCache c(cat, 2, {});
    c.access({0}, 0);
    c.access({0}, 1);
    c.access({0}, 2);  // freq(0) = 3
    c.access({1}, 3);  // freq(1) = 1
    c.access({2}, 4);  // evicts 1 (least frequent)
    auto v = c.contents();
    std::set<int> s(v.begin(), v.end());
    CHECK(s.count(0) == 1 && s.count(2) == 1 && s.count(1) == 0);
}

TEST(gdsf_prefers_small_valuable_objects) {
    Catalog cat = Catalog::fromSizes({100, 1, 1});
    GDSFCache c(cat, 101, {1.0, 0.0});
    c.access({0}, 0);
    c.access({1}, 1);
    c.access({2}, 2);  // must evict: the big object has the lowest benefit/size
    auto v = c.contents();
    std::set<int> s(v.begin(), v.end());
    CHECK(s.count(0) == 0 && s.count(1) == 1 && s.count(2) == 1);
}

TEST(belady_is_optimal_for_unit_sizes) {
    // For unit sizes Belady (with bypass) is optimal; brute-force-check against all
    // eviction choices on small random traces.
    Rng rng(9);
    for (int trial = 0; trial < 200; ++trial) {
        const int n = 5, k = 2;
        std::vector<int> ids;
        for (int i = 0; i < 12; ++i) ids.push_back(static_cast<int>(rng.below(n)));
        Catalog cat = unitCatalog(n);
        Trace tr = manualTrace(ids, n, 12);
        BeladyCache b(cat, k, {});
        b.bindTrace(&tr);
        int hits = 0;
        for (std::size_t i = 0; i < ids.size(); ++i) hits += b.access({ids[i]}, static_cast<i64>(i));
        // exhaustive search over cache states (bypass allowed)
        std::function<int(std::size_t, std::set<int>)> best = [&](std::size_t i, std::set<int> cache) -> int {
            if (i == ids.size()) return 0;
            const int x = ids[i];
            if (cache.count(x)) return 1 + best(i + 1, cache);
            int res = best(i + 1, cache);  // bypass
            if (static_cast<int>(cache.size()) < k) {
                auto c2 = cache;
                c2.insert(x);
                res = std::max(res, best(i + 1, c2));
            } else {
                for (int v : cache) {
                    auto c2 = cache;
                    c2.erase(v);
                    c2.insert(x);
                    res = std::max(res, best(i + 1, c2));
                }
            }
            return res;
        };
        CHECK(hits == best(0, {}));
    }
}

TEST(all_strategies_respect_capacity_and_report_true_hits) {
    SimConfig cfg;
    cfg.numObjects = 400;
    cfg.epochs = 8;
    cfg.epochLen = 2000;
    cfg.capacityFrac = 0.05;
    cfg.traffic = presetTraffic("mixed");
    CatalogConfig cc;
    cc.numObjects = cfg.numObjects;
    Catalog cat = Catalog::synthetic(cc);
    Trace tr = buildTrace(cat, cfg.traffic, cfg.epochs, cfg.epochLen, 3);
    const i64 cap = static_cast<i64>(cfg.capacityFrac * static_cast<double>(cat.totalKB));
    for (const auto& key : allStrategyKeys()) {
        auto s = makeStrategy(key, cat, cap, cfg.weights, cfg.epochLen, 512);
        s->bindTrace(&tr);
        for (int e = 0; e < tr.epochs(); ++e) {
            s->beginEpoch(e);
            for (std::size_t i = tr.epochStart[e]; i < tr.epochStart[e + 1]; ++i) {
                auto before = s->contents();
                std::unordered_set<int> set(before.begin(), before.end());
                const bool claimedHit = s->access(tr.requests[i], static_cast<i64>(i));
                CHECK(claimedHit == (set.count(tr.requests[i].objectId) > 0));
                CHECK(s->usedKB() <= cap);
                // contents() must agree with usedKB()
                if (i % 997 == 0) {
                    i64 sum = 0;
                    for (int id : s->contents()) sum += cat[id].sizeKB;
                    CHECK(sum == s->usedKB());
                }
            }
            s->endEpoch(e);
        }
    }
}

TEST(simulation_engine_runs_and_oracles_lead) {
    SimConfig cfg;
    cfg.numObjects = 800;
    cfg.epochs = 10;
    cfg.epochLen = 4000;
    cfg.capacityFrac = 0.05;
    cfg.traffic = presetTraffic("zipf");
    cfg.algos = {"lru", "dp-reactive", "dp-oracle", "belady", "pack"};
    SimRun run = SimulationEngine::run(cfg);
    CHECK(run.algos.size() == 5);
    double lru = run.algos[0].summary.hitRatio;
    double belady = run.algos[3].summary.hitRatio;
    double pack = run.algos[4].summary.hitRatio;
    CHECK(belady >= lru);
    CHECK(pack > 0.0);
    for (const auto& a : run.algos) CHECK(a.summary.maxUsedFrac <= 1.0 + 1e-12);
    JsonWriter j;
    writeRun(j, run, true);
    CHECK(j.str().size() > 100);
    CHECK(j.str().front() == '{' && j.str().back() == '}');
}
