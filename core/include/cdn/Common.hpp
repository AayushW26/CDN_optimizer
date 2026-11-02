// Common types shared by every module of the CDN Cache Optimizer.
#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cdn {

using i64 = std::int64_t;
using u64 = std::uint64_t;

constexpr i64 kInfTick = std::numeric_limits<i64>::max();

/// Content kinds in the synthetic catalog (affects size distribution only).
enum class ContentKind : int { Static = 0, VideoChunk = 1, LargeFile = 2 };

inline const char* kindName(int k) {
    switch (k) {
        case 0: return "static";
        case 1: return "video";
        case 2: return "large";
        default: return "other";
    }
}

/// A content object in the catalog (the knapsack "item").
struct Object {
    int id = 0;
    i64 sizeKB = 1;          // storage cost (knapsack weight)
    double fetchCost = 0.0;  // $ paid to origin per miss (egress price x size)
    double latencyMs = 0.0;  // extra latency a miss adds (origin RTT / TTFB)
    int originTier = 0;      // which origin (pricing / distance class)
    int kind = 0;            // ContentKind
};

/// One request in the stream.
struct Request {
    int objectId = 0;
};

/// Relative weights of the two pain points a cache hit avoids.
///   B_i = f_i * (alpha * c_i / mean(c) + beta * l_i / mean(l))
struct BenefitWeights {
    double alpha = 1.0;  // bandwidth / $ weight
    double beta = 1.0;   // latency weight
};

/// Monotonic wall-clock helper.
class Stopwatch {
public:
    Stopwatch() : start_(Clock::now()) {}
    void reset() { start_ = Clock::now(); }
    double ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
    }
    i64 ns() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count();
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_;
};

}  // namespace cdn
