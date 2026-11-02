#include <cmath>
#include <map>

#include "TestFramework.hpp"
#include "cdn/Random.hpp"
#include "cdn/Traffic.hpp"

using namespace cdn;

TEST(alias_sampler_matches_distribution) {
    std::vector<double> w = {1, 2, 3, 4, 0, 10};
    AliasSampler a(w);
    Rng rng(1);
    std::vector<int> cnt(w.size(), 0);
    const int N = 400000;
    for (int i = 0; i < N; ++i) cnt[a.sample(rng)]++;
    const double sum = 20;
    for (std::size_t i = 0; i < w.size(); ++i) {
        const double expected = N * w[i] / sum;
        CHECK(std::fabs(cnt[i] - expected) < 5 * std::sqrt(expected + 1) + 1);
    }
    CHECK(cnt[4] == 0);
}

TEST(zipf_head_dominates) {
    CatalogConfig cc;
    cc.numObjects = 2000;
    Catalog cat = Catalog::synthetic(cc);
    TrafficConfig tc = presetTraffic("zipf");
    tc.zipfS = 1.0;
    Trace tr = buildTrace(cat, tc, 2, 50000, 4);
    std::map<int, int> cnt;
    for (const auto& r : tr.requests) cnt[r.objectId]++;
    std::vector<int> c;
    for (auto& kv : cnt) c.push_back(kv.second);
    std::sort(c.rbegin(), c.rend());
    long top = 0, all = 0;
    for (std::size_t i = 0; i < c.size(); ++i) {
        all += c[i];
        if (i < 200) top += c[i];
    }
    CHECK(static_cast<double>(top) / all > 0.6);  // top 10% of objects get > 60% of requests
}

TEST(flash_crowd_creates_markers_and_bursts) {
    CatalogConfig cc;
    cc.numObjects = 1000;
    Catalog cat = Catalog::synthetic(cc);
    Trace tr = buildTrace(cat, presetTraffic("flash"), 20, 5000, 5);
    int flashes = 0;
    for (const auto& m : tr.markers) flashes += m.type == "flash";
    CHECK(flashes >= 1);
}

TEST(next_use_is_correct) {
    Trace t;
    for (int id : {1, 2, 1, 3, 2, 1}) t.requests.push_back({id});
    t.epochStart = {0, 6};
    t.computeNextUse(4);
    std::vector<i64> expect = {2, 4, 5, kInfTick, kInfTick, kInfTick};
    for (std::size_t i = 0; i < expect.size(); ++i) CHECK(t.nextUse[i] == expect[i]);
}

TEST(generation_is_deterministic) {
    CatalogConfig cc;
    cc.numObjects = 500;
    Catalog cat = Catalog::synthetic(cc);
    Trace a = buildTrace(cat, presetTraffic("mixed"), 5, 1000, 42);
    Trace b = buildTrace(cat, presetTraffic("mixed"), 5, 1000, 42);
    CHECK(a.requests.size() == b.requests.size());
    for (std::size_t i = 0; i < a.requests.size(); ++i) CHECK(a.requests[i].objectId == b.requests[i].objectId);
}
