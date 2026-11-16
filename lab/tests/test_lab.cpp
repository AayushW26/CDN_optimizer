// Unit and property tests for the cache lab. Run: ./build/cachelab_tests
#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "bounds.hpp"
#include "registry.hpp"

using namespace lab;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(cond)) {                                                              \
            ++failures;                                                             \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                           \
    } while (0)

static Trace makeTrace(const std::vector<uint32_t>& ids, const std::vector<uint32_t>& sizes) {
    Trace t;
    t.id = ids;
    t.size = sizes;
    uint32_t mx = 0;
    for (uint32_t x : ids) mx = std::max(mx, x + 1);
    t.numObjects = mx;
    t.computeDerived();
    return t;
}

static std::vector<bool> replay(const std::string& policy, const Trace& t, uint64_t cap) {
    PolicyContext ctx;
    ctx.trace = &t;
    ctx.capacity = cap;
    auto p = makePolicy(policy, ctx);
    std::vector<bool> hits;
    for (std::size_t i = 0; i < t.n(); ++i) hits.push_back(p->access(i, t.id[i], t.size[i]));
    return hits;
}

// Zipf-ish random trace with variable sizes and a scan component (one-hit wonders).
static Trace randomTrace(uint64_t seed, std::size_t n, uint32_t objects) {
    Rng r(seed);
    std::vector<uint32_t> ids, sizes, objSize(objects + n);
    for (auto& s : objSize) s = 1 + static_cast<uint32_t>(std::exp(r.uniform() * 9.0));  // 1 .. 8000 bytes
    std::vector<double> cdf(objects);
    double acc = 0;
    for (uint32_t k = 0; k < objects; ++k) cdf[k] = (acc += 1.0 / std::pow(k + 1.0, 0.9));
    uint32_t scanNext = objects;
    for (std::size_t i = 0; i < n; ++i) {
        uint32_t x;
        if (r.uniform() < 0.3) x = scanNext++;  // never repeats
        else {
            const double u = r.uniform() * acc;
            x = static_cast<uint32_t>(std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin());
        }
        ids.push_back(x);
        sizes.push_back(objSize[x]);
    }
    return makeTrace(ids, sizes);
}

static void test_trace_next_and_footprint() {
    Trace t = makeTrace({0, 1, 0, 2, 1, 0}, {10, 20, 10, 30, 20, 10});
    CHECK(t.next[0] == 2);
    CHECK(t.next[1] == 4);
    CHECK(t.next[2] == 5);
    CHECK(t.next[3] == kNever);
    CHECK(t.footprintBytes == 60);
    CHECK(t.totalBytes == 100);
}

static void test_convert_and_binary_roundtrip() {
    std::stringstream in("0 5 100\n1 -7 50\n2 5 100\n3 5 0\n4 5 200\nbad line\n");
    ConvertStats st;
    Trace t = convertText(in, st);
    CHECK(st.kept == 4);
    CHECK(st.zeroSize == 1);
    CHECK(st.malformed == 1);
    CHECK(t.numObjects == 3);  // (5,100), (-7,50), (5,200)
    CHECK(t.id[0] == t.id[2]);
    CHECK(t.id[3] != t.id[0]);
    const std::string path = "/tmp/cachelab_test_trace.bin";
    saveBinary(t, path);
    Trace u = loadBinary(path);
    CHECK(u.n() == 4);
    CHECK(u.id[0] == u.id[2]);
    CHECK(u.size[3] == 200);
    Trace v = loadBinary(path, 1, 2);  // slice
    CHECK(v.n() == 2);
    CHECK(v.size[0] == 50 && v.size[1] == 100);
}

static void test_lru_fifo_exact() {
    // unit sizes, capacity 3:  a b c a d b
    Trace t = makeTrace({0, 1, 2, 0, 3, 1}, {1, 1, 1, 1, 1, 1});
    auto lru = replay("LRU", t, 3);
    CHECK(lru == std::vector<bool>({false, false, false, true, false, false}));  // d evicts b
    auto fifo = replay("FIFO", t, 3);
    CHECK(fifo == std::vector<bool>({false, false, false, true, false, true}));  // d evicts a, b stays
}

static void test_sieve_exact() {
    // capacity 3: a b c a(visited) d -> hand starts at oldest (a): visited -> clear, move to b: evict b
    Trace t = makeTrace({0, 1, 2, 0, 3, 1, 0}, {1, 1, 1, 1, 1, 1, 1});
    auto h = replay("SIEVE", t, 3);
    CHECK(h[3] == true);
    CHECK(h[5] == false);  // b was evicted
}

static void test_s3fifo_filters_one_hit_wonders() {
    // A hot object requested repeatedly survives a long scan of one-time objects.
    std::vector<uint32_t> ids, sizes;
    for (int i = 0; i < 4; ++i) { ids.push_back(0); sizes.push_back(1); }
    for (uint32_t k = 1; k <= 200; ++k) { ids.push_back(k); sizes.push_back(1); }
    ids.push_back(0); sizes.push_back(1);
    Trace t = makeTrace(ids, sizes);
    auto s3 = replay("S3-FIFO", t, 20);
    auto lru = replay("LRU", t, 20);
    CHECK(s3.back() == true);
    CHECK(lru.back() == false);
}

static void test_gdsf_prefers_small() {
    // capacity 10: big(8) small(2) then a new object of size 2 must evict the big one
    Trace t = makeTrace({0, 1, 2, 1, 0}, {8, 2, 2, 2, 8});
    auto h = replay("GDSF", t, 10);
    CHECK(h[3] == true);   // small survived
    CHECK(h[4] == false);  // big was evicted
}

static void test_pfoo_exact_small() {
    // a(1) b(1) a b, capacity 1, N = 4 -> budget 4 space-time; intervals a:[0,2) cost 2, b:[1,3) cost 2
    Trace t = makeTrace({0, 1, 0, 1}, {1, 1, 1, 1});
    BoundResult b = pfooL(t, 1);
    CHECK(std::fabs(b.hitRatio - 0.5) < 1e-12);  // both intervals fit: 2 hits / 4
}

static void test_all_policies_capacity_and_bound() {
    for (uint64_t seed : {1ull, 2ull, 3ull}) {
        Trace t = randomTrace(seed, 60000, 3000);
        for (double frac : {0.01, 0.05, 0.2}) {
            const uint64_t cap = static_cast<uint64_t>(frac * t.footprintBytes);
            const BoundResult b = pfooL(t, cap);
            std::vector<std::string> names = allPolicyNames();
            names.push_back("SHADE-B");
            names.push_back("SHADE-noAdmit");
            for (const auto& name : names) {
                PolicyContext ctx;
                ctx.trace = &t;
                ctx.capacity = cap;
                auto p = makePolicy(name, ctx);
                std::vector<char> seen(t.numObjects, 0);
                uint64_t hits = 0, hitBytes = 0;
                bool capOk = true, causal = true;
                for (std::size_t i = 0; i < t.n(); ++i) {
                    const bool h = p->access(i, t.id[i], t.size[i]);
                    if (h && !seen[t.id[i]]) causal = false;  // cannot hit on a first request
                    seen[t.id[i]] = 1;
                    if (h) { ++hits; hitBytes += t.size[i]; }
                    if (p->usedBytes() > cap) capOk = false;
                }
                CHECK(capOk);
                CHECK(causal);
                CHECK(double(hits) / t.n() <= b.hitRatio + 1e-9);                   // PFOO-L is an upper bound
                CHECK(double(hitBytes) / double(t.totalBytes) <= b.byteHitRatio + 1e-9);
                if (!capOk || !causal || double(hits) / t.n() > b.hitRatio + 1e-9)
                    std::printf("    policy %s seed %llu frac %.2f\n", name.c_str(), (unsigned long long)seed, frac);
            }
        }
    }
}

static void test_shade_beats_lru_on_scan_mix() {
    Trace t = randomTrace(7, 1500000, 20000);  // long enough for several model fits (every 2^18 requests)
    const uint64_t cap = static_cast<uint64_t>(0.05 * t.footprintBytes);
    auto count = [&](const std::string& p) {
        auto h = replay(p, t, cap);
        return std::count(h.begin(), h.end(), true);
    };
    CHECK(count("SHADE") > count("LRU"));
    CHECK(count("SHADE") > count("SHADE-noAdmit"));  // admission control helps under scans
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"trace_next_and_footprint", test_trace_next_and_footprint},
        {"convert_and_binary_roundtrip", test_convert_and_binary_roundtrip},
        {"lru_fifo_exact", test_lru_fifo_exact},
        {"sieve_exact", test_sieve_exact},
        {"s3fifo_filters_one_hit_wonders", test_s3fifo_filters_one_hit_wonders},
        {"gdsf_prefers_small", test_gdsf_prefers_small},
        {"pfoo_exact_small", test_pfoo_exact_small},
        {"all_policies_capacity_causality_and_bound", test_all_policies_capacity_and_bound},
        {"shade_beats_lru_on_scan_mix", test_shade_beats_lru_on_scan_mix},
    };
    for (auto& [name, fn] : tests) {
        const int before = failures;
        fn();
        std::printf("[ %s ] %s\n", failures == before ? "PASS" : "FAIL", name);
    }
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
