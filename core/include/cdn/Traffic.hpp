// Workload generation: Uniform, Zipf, Flash-Crowd, Drifting popularity and a
// "Mixed" realistic CDN preset (drift + content lifecycles + flash crowds +
// crawler scans), plus a CSV trace loader for real access logs.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cdn/Catalog.hpp"
#include "cdn/Common.hpp"
#include "cdn/Random.hpp"

namespace cdn {

struct TrafficConfig {
    std::string pattern = "zipf";  // uniform | zipf | flash | drift | mixed | trace
    double zipfS = 0.9;

    // Drift: popularity churn of the head + content "lifecycles" (rise, peak, fade).
    double swapRate = 0.0;     // fraction of the top-K ranks swapped with random ranks per epoch
    int risersPerEpoch = 0;    // new trending objects born each epoch
    double riserTimeToPeak = 2.5;  // epochs from birth to peak popularity
    int riserPeakRankMax = 40;     // a riser peaks with the weight of a rank in [0, max]

    // Flash crowds: sudden burst of traffic to a small set of previously-cold objects.
    double flashIntensity = 0.0;   // peak share of all requests going to the hot set
    int flashEvery = 12;           // mean epochs between bursts
    double flashDuration = 4.0;    // epochs until the burst has faded
    int hotSetSize = 25;

    // Crawler scans: one-hit-wonder sequential sweeps that pollute recency caches.
    double scanFrac = 0.0;
    int scanEvery = 15;

    std::string tracePath;  // for pattern == "trace"
};

/// Fill a TrafficConfig with the named preset (keeps zipfS / intensity overrides
/// if `keepOverrides` holds non-default values).
TrafficConfig presetTraffic(const std::string& pattern);

struct Marker {
    int epoch = 0;
    double start = 0;  // fractional epoch time
    std::string type;  // flash | scan
    std::string label;
};

struct Trace {
    std::vector<Request> requests;
    std::vector<std::size_t> epochStart;  // size = epochs + 1
    std::vector<i64> nextUse;             // global index of the next request to the same object
    std::vector<Marker> markers;

    int epochs() const { return static_cast<int>(epochStart.size()) - 1; }
    void computeNextUse(std::size_t numObjects);
};

class WorkloadGenerator {
public:
    WorkloadGenerator(const Catalog& catalog, TrafficConfig cfg, std::uint64_t seed);
    std::vector<Request> generateEpoch(int epoch, int count);
    const std::vector<Marker>& markers() const { return markers_; }
    const TrafficConfig& config() const { return cfg_; }

private:
    struct Riser {
        int obj;
        double birth;
        double peak;
    };
    struct Flash {
        double start;
        std::vector<int> hot;
        AliasSampler sampler;
    };

    void evolve(int epoch);
    double flashShare(double t, const Flash*& active) const;
    bool scanActive(int epoch) const;

    const Catalog& cat_;
    TrafficConfig cfg_;
    Rng rng_;
    std::vector<int> rankToObj_;
    std::vector<double> zipfW_;
    std::vector<Riser> risers_;
    std::vector<Flash> flashes_;
    std::vector<int> scanEpochs_;
    std::vector<Marker> markers_;
    int scanCursor_ = 0;
    int lastEvolved_ = -1;
};

/// Generate a full trace (all epochs) so every strategy replays the *same* stream.
Trace buildTrace(const Catalog& catalog, const TrafficConfig& cfg, int epochs, int epochLen,
                 std::uint64_t seed);

/// Load a CSV access log. Accepted headers: "object_id,size_kb" or
/// "timestamp,object_id,size_kb". Builds the catalog from the sizes seen.
Trace loadTraceCsv(const std::string& path, int epochLen, Catalog& catalogOut);

}  // namespace cdn
