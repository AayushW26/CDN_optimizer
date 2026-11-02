#include "cdn/Traffic.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace cdn {

TrafficConfig presetTraffic(const std::string& pattern) {
    TrafficConfig c;
    c.pattern = pattern;
    if (pattern == "uniform" || pattern == "zipf" || pattern == "trace") {
        // nothing extra
    } else if (pattern == "flash") {
        c.flashIntensity = 0.45;
        c.flashEvery = 10;
        c.flashDuration = 4.0;
        c.hotSetSize = 25;
    } else if (pattern == "drift") {
        c.swapRate = 0.10;
        c.risersPerEpoch = 6;
        c.riserTimeToPeak = 2.5;
        c.riserPeakRankMax = 40;
    } else if (pattern == "mixed") {
        c.zipfS = 0.85;
        c.swapRate = 0.05;
        c.risersPerEpoch = 4;
        c.flashIntensity = 0.35;
        c.flashEvery = 13;
        c.scanFrac = 0.15;
        c.scanEvery = 11;
    } else {
        throw std::invalid_argument("unknown traffic pattern: " + pattern);
    }
    return c;
}

void Trace::computeNextUse(std::size_t numObjects) {
    nextUse.assign(requests.size(), kInfTick);
    std::vector<i64> last(numObjects, kInfTick);
    for (std::size_t i = requests.size(); i-- > 0;) {
        const auto id = static_cast<std::size_t>(requests[i].objectId);
        nextUse[i] = last[id];
        last[id] = static_cast<i64>(i);
    }
}

WorkloadGenerator::WorkloadGenerator(const Catalog& catalog, TrafficConfig cfg, std::uint64_t seed)
    : cat_(catalog), cfg_(std::move(cfg)), rng_(seed) {
    const int n = static_cast<int>(cat_.size());
    rankToObj_.resize(static_cast<std::size_t>(n));
    std::iota(rankToObj_.begin(), rankToObj_.end(), 0);
    rng_.shuffle(rankToObj_);  // popularity independent of size/origin
    zipfW_.resize(static_cast<std::size_t>(n));
    for (int r = 0; r < n; ++r)
        zipfW_[static_cast<std::size_t>(r)] =
            cfg_.pattern == "uniform" ? 1.0 : 1.0 / std::pow(r + 1.0, cfg_.zipfS);

    // Pre-schedule flash crowds (hot sets drawn from the cold half of the catalog).
    if (cfg_.flashIntensity > 0) {
        double t = cfg_.flashEvery * 0.5 + rng_.uniform(0.1, 0.8);
        while (t < 100000) {
            Flash f;
            f.start = t;
            std::vector<double> w;
            for (int k = 0; k < cfg_.hotSetSize; ++k) {
                int rank = n / 2 + static_cast<int>(rng_.below(static_cast<std::uint64_t>(n - n / 2)));
                f.hot.push_back(rankToObj_[static_cast<std::size_t>(rank)]);
                w.push_back(1.0 / std::pow(k + 1.0, 0.8));
            }
            f.sampler.build(w);
            flashes_.push_back(std::move(f));
            t += cfg_.flashEvery * rng_.uniform(0.75, 1.25);
            if (flashes_.size() > 2000) break;
        }
    }
    if (cfg_.scanFrac > 0) {
        for (int e = cfg_.scanEvery / 2 + 1; e < 100000; e += cfg_.scanEvery) scanEpochs_.push_back(e);
    }
}

void WorkloadGenerator::evolve(int epoch) {
    const int n = static_cast<int>(cat_.size());
    while (lastEvolved_ < epoch) {
        ++lastEvolved_;
        const int e = lastEvolved_;
        if (e == 0) continue;
        // 1) churn of the head: swap some top-K ranks with random ranks
        const int topK = std::max(10, n / 20);
        const int swaps = static_cast<int>(std::round(cfg_.swapRate * topK));
        for (int s = 0; s < swaps; ++s) {
            auto a = rng_.below(static_cast<std::uint64_t>(topK));
            auto b = rng_.below(static_cast<std::uint64_t>(n));
            std::swap(rankToObj_[a], rankToObj_[b]);
        }
        // 2) new trending content (lifecycle: rise -> peak -> fade)
        for (int k = 0; k < cfg_.risersPerEpoch; ++k) {
            int rank = n / 3 + static_cast<int>(rng_.below(static_cast<std::uint64_t>(n - n / 3)));
            int peakRank = static_cast<int>(rng_.below(static_cast<std::uint64_t>(cfg_.riserPeakRankMax + 1)));
            risers_.push_back({rankToObj_[static_cast<std::size_t>(rank)], e + rng_.uniform(),
                               zipfW_[static_cast<std::size_t>(std::min(peakRank, n - 1))]});
        }
        // forget risers that have faded
        risers_.erase(std::remove_if(risers_.begin(), risers_.end(),
                                     [&](const Riser& r) { return e - r.birth > 6 * cfg_.riserTimeToPeak; }),
                      risers_.end());
    }
}

double WorkloadGenerator::flashShare(double t, const Flash*& active) const {
    active = nullptr;
    double best = 0;
    for (const Flash& f : flashes_) {
        if (f.start > t) break;
        double tau = t - f.start;
        if (tau > cfg_.flashDuration) continue;
        constexpr double ramp = 0.25;
        double s = tau < ramp ? cfg_.flashIntensity * tau / ramp
                              : cfg_.flashIntensity * std::exp(-(tau - ramp) / (cfg_.flashDuration / 3.0));
        if (s > best) {
            best = s;
            active = &f;
        }
    }
    return best;
}

bool WorkloadGenerator::scanActive(int epoch) const {
    return std::binary_search(scanEpochs_.begin(), scanEpochs_.end(), epoch);
}

std::vector<Request> WorkloadGenerator::generateEpoch(int epoch, int count) {
    evolve(epoch);
    const std::size_t n = cat_.size();
    constexpr int kPhases = 10;  // popularity is re-evaluated 10x per epoch
    std::vector<Request> out;
    out.reserve(static_cast<std::size_t>(count));

    // markers (once per event)
    for (const Flash& f : flashes_) {
        if (static_cast<int>(std::floor(f.start)) == epoch)
            markers_.push_back({epoch, f.start, "flash", "flash crowd (" + std::to_string(f.hot.size()) + " objects)"});
    }
    const bool scanning = scanActive(epoch);
    if (scanning) markers_.push_back({epoch, static_cast<double>(epoch), "scan", "crawler scan"});

    std::vector<double> w(n);
    for (int ph = 0; ph < kPhases; ++ph) {
        const double t = epoch + (ph + 0.5) / kPhases;
        for (std::size_t r = 0; r < n; ++r) w[static_cast<std::size_t>(rankToObj_[r])] = zipfW_[r];
        for (const Riser& rs : risers_) {
            double age = t - rs.birth;
            if (age <= 0) continue;
            double x = age / cfg_.riserTimeToPeak;
            w[static_cast<std::size_t>(rs.obj)] += rs.peak * x * std::exp(1.0 - x);
        }
        AliasSampler base(w);
        const Flash* fl = nullptr;
        const double fShare = flashShare(t, fl);
        const double sShare = scanning ? cfg_.scanFrac : 0.0;

        const int lo = count * ph / kPhases;
        const int hi = count * (ph + 1) / kPhases;
        for (int k = lo; k < hi; ++k) {
            const double u = rng_.uniform();
            int obj;
            if (fl && u < fShare) {
                obj = fl->hot[fl->sampler.sample(rng_)];
            } else if (u < fShare + sShare) {
                obj = scanCursor_;
                scanCursor_ = (scanCursor_ + 1) % static_cast<int>(n);
            } else {
                obj = static_cast<int>(base.sample(rng_));
            }
            out.push_back({obj});
        }
    }
    return out;
}

Trace buildTrace(const Catalog& catalog, const TrafficConfig& cfg, int epochs, int epochLen,
                 std::uint64_t seed) {
    if (epochs <= 0 || epochLen <= 0) throw std::invalid_argument("buildTrace: epochs/epochLen must be > 0");
    WorkloadGenerator gen(catalog, cfg, seed);
    Trace tr;
    tr.requests.reserve(static_cast<std::size_t>(epochs) * static_cast<std::size_t>(epochLen));
    tr.epochStart.push_back(0);
    for (int e = 0; e < epochs; ++e) {
        auto reqs = gen.generateEpoch(e, epochLen);
        tr.requests.insert(tr.requests.end(), reqs.begin(), reqs.end());
        tr.epochStart.push_back(tr.requests.size());
    }
    tr.markers = gen.markers();
    tr.computeNextUse(catalog.size());
    return tr;
}

Trace loadTraceCsv(const std::string& path, int epochLen, Catalog& catalogOut) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open trace csv: " + path);
    std::string header;
    std::getline(in, header);
    std::vector<std::string> cols;
    {
        std::stringstream hs(header);
        std::string c;
        while (std::getline(hs, c, ',')) {
            while (!c.empty() && (c.back() == '\r' || c.back() == ' ')) c.pop_back();
            cols.push_back(c);
        }
    }
    int idCol = -1, sizeCol = -1;
    for (int i = 0; i < static_cast<int>(cols.size()); ++i) {
        if (cols[i] == "object_id" || cols[i] == "id") idCol = i;
        if (cols[i] == "size_kb" || cols[i] == "size") sizeCol = i;
    }
    if (idCol < 0 || sizeCol < 0) throw std::runtime_error("trace csv needs object_id and size_kb columns");
    std::unordered_map<std::string, int> remap;
    std::vector<i64> sizes;
    Trace tr;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string cell;
        std::string idStr;
        i64 sz = 1;
        for (int i = 0; std::getline(ss, cell, ','); ++i) {
            if (i == idCol) idStr = cell;
            if (i == sizeCol) sz = std::max<i64>(1, std::llround(std::stod(cell)));
        }
        auto [it, inserted] = remap.emplace(idStr, static_cast<int>(sizes.size()));
        if (inserted) sizes.push_back(sz);
        tr.requests.push_back({it->second});
    }
    if (tr.requests.empty()) throw std::runtime_error("trace csv has no requests: " + path);
    catalogOut = Catalog::fromSizes(sizes);
    for (std::size_t s = 0; s < tr.requests.size(); s += static_cast<std::size_t>(epochLen))
        tr.epochStart.push_back(s);
    tr.epochStart.push_back(tr.requests.size());
    tr.computeNextUse(catalogOut.size());
    return tr;
}

}  // namespace cdn
