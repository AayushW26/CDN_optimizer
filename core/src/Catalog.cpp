#include "cdn/Catalog.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace cdn {

const std::vector<OriginTier>& originTiers() {
    static const std::vector<OriginTier> tiers = {
        {"on-prem-dc", 0.02, 35.0, 0.25},
        {"cloud-same-region", 0.09, 60.0, 0.40},
        {"cloud-cross-region", 0.12, 140.0, 0.25},
        {"third-party-premium", 0.25, 260.0, 0.10},
    };
    return tiers;
}

namespace {

int drawTier(Rng& rng) {
    double u = rng.uniform();
    const auto& t = originTiers();
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (u < t[i].share) return static_cast<int>(i);
        u -= t[i].share;
    }
    return static_cast<int>(t.size()) - 1;
}

void priceObject(Object& o, Rng& rng) {
    const OriginTier& t = originTiers()[static_cast<std::size_t>(o.originTier)];
    const double gb = static_cast<double>(o.sizeKB) / (1024.0 * 1024.0);
    o.fetchCost = gb * t.dollarsPerGB;
    o.latencyMs = t.rttMs * rng.lognormal(0.0, 0.25);
}

i64 clampSize(double kb, double lo, double hi) {
    return static_cast<i64>(std::llround(std::clamp(kb, lo, hi)));
}

}  // namespace

void Catalog::finalize() {
    totalKB = 0;
    double c = 0, l = 0;
    for (const auto& o : objects) {
        totalKB += o.sizeKB;
        c += o.fetchCost;
        l += o.latencyMs;
    }
    const double n = std::max<double>(1.0, static_cast<double>(objects.size()));
    meanCost = c > 0 ? c / n : 1.0;
    meanLatency = l > 0 ? l / n : 1.0;
}

Catalog Catalog::synthetic(const CatalogConfig& cfg) {
    if (cfg.numObjects <= 0) throw std::invalid_argument("catalog: numObjects must be > 0");
    Rng rng(cfg.seed);
    Catalog cat;
    cat.objects.resize(static_cast<std::size_t>(cfg.numObjects));
    for (int i = 0; i < cfg.numObjects; ++i) {
        Object& o = cat.objects[static_cast<std::size_t>(i)];
        o.id = i;
        const double u = rng.uniform();
        if (u < cfg.staticFrac) {
            o.kind = static_cast<int>(ContentKind::Static);
            o.sizeKB = clampSize(rng.lognormal(std::log(60.0), 1.0), 2, 4096);
        } else if (u < cfg.staticFrac + cfg.videoFrac) {
            o.kind = static_cast<int>(ContentKind::VideoChunk);
            o.sizeKB = clampSize(rng.lognormal(std::log(2500.0), 0.6), 256, 16384);
        } else {
            o.kind = static_cast<int>(ContentKind::LargeFile);
            o.sizeKB = clampSize(rng.lognormal(std::log(60000.0), 0.8), 8192, 512000);
        }
        o.originTier = drawTier(rng);
        priceObject(o, rng);
    }
    cat.finalize();
    return cat;
}

Catalog Catalog::fromSizes(const std::vector<i64>& sizesKB, std::uint64_t seed) {
    Rng rng(seed);
    Catalog cat;
    cat.objects.resize(sizesKB.size());
    for (std::size_t i = 0; i < sizesKB.size(); ++i) {
        Object& o = cat.objects[i];
        o.id = static_cast<int>(i);
        o.sizeKB = std::max<i64>(1, sizesKB[i]);
        o.kind = o.sizeKB < 1024 ? 0 : (o.sizeKB < 16384 ? 1 : 2);
        o.originTier = drawTier(rng);
        priceObject(o, rng);
    }
    cat.finalize();
    return cat;
}

Catalog Catalog::fromCsv(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open catalog csv: " + path);
    std::string line;
    std::getline(in, line);  // header
    Catalog cat;
    Rng rng(11);
    std::unordered_map<std::string, int> ids;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string idStr, sz, cost, lat;
        std::getline(ss, idStr, ',');
        std::getline(ss, sz, ',');
        std::getline(ss, cost, ',');
        std::getline(ss, lat, ',');
        if (sz.empty()) continue;
        Object o;
        o.id = static_cast<int>(cat.objects.size());
        ids[idStr] = o.id;
        o.sizeKB = std::max<i64>(1, std::stoll(sz));
        o.kind = o.sizeKB < 1024 ? 0 : (o.sizeKB < 16384 ? 1 : 2);
        o.originTier = drawTier(rng);
        priceObject(o, rng);
        if (!cost.empty()) o.fetchCost = std::stod(cost);
        if (!lat.empty()) o.latencyMs = std::stod(lat);
        cat.objects.push_back(o);
    }
    if (cat.objects.empty()) throw std::runtime_error("catalog csv has no rows: " + path);
    cat.finalize();
    return cat;
}

}  // namespace cdn
