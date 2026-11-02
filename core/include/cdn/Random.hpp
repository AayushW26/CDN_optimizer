// Deterministic, fast PRNG (xoshiro256**) + Vose's alias method sampler.
//
// The alias method gives O(1) sampling from any discrete distribution after
// O(n) preprocessing -- this is what lets the Zipf generator emit millions of
// requests per second (vs. O(log n) for inverse-CDF + binary search).
#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace cdn {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 42) { reseed(seed); }

    void reseed(std::uint64_t seed) {
        std::uint64_t x = seed;
        for (auto& si : s_) si = splitmix(x);
    }

    std::uint64_t next() {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    /// Uniform double in [0, 1).
    double uniform() { return (next() >> 11) * 0x1.0p-53; }
    double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

    /// Uniform integer in [0, n).
    std::uint64_t below(std::uint64_t n) {
        if (n == 0) throw std::invalid_argument("Rng::below(0)");
        // Lemire's nearly-divisionless method.
        __uint128_t m = static_cast<__uint128_t>(next()) * n;
        auto l = static_cast<std::uint64_t>(m);
        if (l < n) {
            std::uint64_t t = (0 - n) % n;
            while (l < t) {
                m = static_cast<__uint128_t>(next()) * n;
                l = static_cast<std::uint64_t>(m);
            }
        }
        return static_cast<std::uint64_t>(m >> 64);
    }

    int intIn(int lo, int hi) {  // inclusive
        return lo + static_cast<int>(below(static_cast<std::uint64_t>(hi - lo + 1)));
    }

    double normal() {  // Box-Muller
        double u1 = uniform();
        double u2 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }

    double lognormal(double mu, double sigma) { return std::exp(mu + sigma * normal()); }

    template <class T>
    void shuffle(std::vector<T>& v) {
        for (std::size_t i = v.size(); i > 1; --i) {
            std::size_t j = below(i);
            std::swap(v[i - 1], v[j]);
        }
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    static std::uint64_t splitmix(std::uint64_t& x) {
        std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    std::uint64_t s_[4]{};
};

/// Vose's alias method: O(n) build, O(1) sample.
class AliasSampler {
public:
    AliasSampler() = default;
    explicit AliasSampler(const std::vector<double>& weights) { build(weights); }

    void build(const std::vector<double>& weights) {
        const std::size_t n = weights.size();
        if (n == 0) throw std::invalid_argument("AliasSampler: empty weights");
        prob_.assign(n, 0.0);
        alias_.assign(n, 0);
        double sum = 0;
        for (double w : weights) {
            if (w < 0) throw std::invalid_argument("AliasSampler: negative weight");
            sum += w;
        }
        if (sum <= 0) throw std::invalid_argument("AliasSampler: zero total weight");
        std::vector<double> scaled(n);
        std::vector<std::uint32_t> small, large;
        small.reserve(n);
        large.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            scaled[i] = weights[i] * static_cast<double>(n) / sum;
            (scaled[i] < 1.0 ? small : large).push_back(static_cast<std::uint32_t>(i));
        }
        while (!small.empty() && !large.empty()) {
            std::uint32_t s = small.back();
            small.pop_back();
            std::uint32_t l = large.back();
            prob_[s] = scaled[s];
            alias_[s] = l;
            scaled[l] = (scaled[l] + scaled[s]) - 1.0;
            if (scaled[l] < 1.0) {
                large.pop_back();
                small.push_back(l);
            }
        }
        for (auto i : large) prob_[i] = 1.0;
        for (auto i : small) prob_[i] = 1.0;  // numerical leftovers
    }

    std::size_t sample(Rng& rng) const {
        std::size_t i = rng.below(prob_.size());
        return rng.uniform() < prob_[i] ? i : alias_[i];
    }

    std::size_t size() const { return prob_.size(); }

private:
    std::vector<double> prob_;
    std::vector<std::uint32_t> alias_;
};

}  // namespace cdn
