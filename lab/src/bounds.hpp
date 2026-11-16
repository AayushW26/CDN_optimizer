// Offline upper bounds on achievable hit ratio for variable-size caching.
//
// PFOO-L (Berger, Beckmann, Harchol-Balter, SIGMETRICS'18) relaxes optimal caching into a
// FRACTIONAL KNAPSACK over reuse intervals. Every pair of consecutive requests (i, next(i))
// to the same object is an item: caching the object across that interval yields one hit
// (or size bytes for the byte metric) and consumes size x (next(i) - i) of cache
// space-time. The whole run offers capacity x N space-time. Taking items greedily by value
// per unit of space-time (the fractional-knapsack optimum) gives an upper bound on hits of
// ANY policy, offline or online.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "trace.hpp"

namespace lab {

struct BoundResult {
    double hitRatio = 0;      // object hit-ratio upper bound
    double byteHitRatio = 0;  // byte hit-ratio upper bound
};

inline BoundResult pfooL(const Trace& t, uint64_t capacity) {
    const std::size_t N = t.n();
    struct Iv {
        double cost;  // space-time
        uint32_t size;
        uint32_t len;
    };
    std::vector<Iv> iv;
    iv.reserve(N / 2);
    for (std::size_t i = 0; i < N; ++i) {
        if (t.next[i] == kNever || t.size[i] > capacity) continue;
        const uint32_t len = t.next[i] - static_cast<uint32_t>(i);
        iv.push_back({static_cast<double>(t.size[i]) * len, t.size[i], len});
    }
    const double budget = static_cast<double>(capacity) * static_cast<double>(N);
    BoundResult r;
    // Object hits: value 1, weight size*len -> take smallest space-time first.
    std::sort(iv.begin(), iv.end(), [](const Iv& a, const Iv& b) { return a.cost < b.cost; });
    {
        double used = 0, hits = 0;
        for (const Iv& v : iv) {
            if (used + v.cost > budget) {
                hits += (budget - used) / v.cost;  // fractional item
                break;
            }
            used += v.cost;
            hits += 1;
        }
        r.hitRatio = hits / static_cast<double>(N);
    }
    // Byte hits: value size, weight size*len -> take shortest intervals first.
    std::sort(iv.begin(), iv.end(), [](const Iv& a, const Iv& b) { return a.len < b.len; });
    {
        double used = 0, bytes = 0;
        for (const Iv& v : iv) {
            if (used + v.cost > budget) {
                bytes += v.size * (budget - used) / v.cost;
                break;
            }
            used += v.cost;
            bytes += v.size;
        }
        r.byteHitRatio = bytes / static_cast<double>(t.totalBytes);
    }
    return r;
}

}  // namespace lab
