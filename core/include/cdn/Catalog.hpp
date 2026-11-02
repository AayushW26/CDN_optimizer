// Content catalog: the universe of objects an edge server may be asked for.
#pragma once

#include <string>
#include <vector>

#include "cdn/Common.hpp"
#include "cdn/Random.hpp"

namespace cdn {

/// An origin class. Objects pulled from farther / pricier origins are worth
/// more to cache, which is what makes the knapsack values non-trivial.
struct OriginTier {
    const char* name;
    double dollarsPerGB;  // origin egress price
    double rttMs;         // extra time-to-first-byte on a miss
    double share;         // fraction of the catalog served by this origin
};

const std::vector<OriginTier>& originTiers();

struct CatalogConfig {
    int numObjects = 5000;
    double staticFrac = 0.60;  // images, JS, CSS, thumbnails
    double videoFrac = 0.32;   // HLS/DASH video segments
    // remainder: large downloads (installers, game patches, full videos)
    std::uint64_t seed = 7;
};

class Catalog {
public:
    std::vector<Object> objects;
    i64 totalKB = 0;
    double meanCost = 1.0;
    double meanLatency = 1.0;

    static Catalog synthetic(const CatalogConfig& cfg);
    /// CSV with header: id,size_kb[,fetch_cost,latency_ms]. Ids are remapped to 0..n-1.
    static Catalog fromCsv(const std::string& path);
    /// Build from explicit sizes (used by the trace loader and tests).
    static Catalog fromSizes(const std::vector<i64>& sizesKB, std::uint64_t seed = 7);

    std::size_t size() const { return objects.size(); }
    const Object& operator[](int id) const { return objects[static_cast<std::size_t>(id)]; }

    /// Benefit of one cache hit on object o (normalised so alpha/beta are unit-free).
    double hitBenefit(const Object& o, const BenefitWeights& w) const {
        return w.alpha * o.fetchCost / meanCost + w.beta * o.latencyMs / meanLatency;
    }
    /// Cost of fetching o proactively (bandwidth only -- prefetch is off the critical path).
    double admissionCost(const Object& o, const BenefitWeights& w) const {
        return w.alpha * o.fetchCost / meanCost;
    }

    void finalize();  // recompute totals / means
};

}  // namespace cdn
