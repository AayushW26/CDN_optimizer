// SHADE: Shadow-price Admission and hit-Density Eviction (this project's method).
//
// Derivation (see docs/METHOD.md):
//   The PFOO-L relaxation of optimal variable-size caching is a fractional knapsack over
//   reuse intervals: a hit across an interval of length D costs size*D of cache
//   space-time, and the optimum keeps exactly the hits whose value per space-time exceeds
//   one shadow price lambda (the LP dual of the space-time capacity constraint).
//   Online, D is unknown. SHADE therefore
//     1. learns, from a policy-independent request history (cached AND uncached objects),
//        the distribution of time-to-next-request conditioned on a small feature class
//        (request count k, gap between the last two requests);
//     2. turns it into a hit density HD(class, age) = P(hit before horizon | age) /
//        E[remaining space-time | age]  (hits per byte per request when divided by size);
//     3. evicts the sampled cached object with the lowest density, and tracks the
//        victims' density as the running shadow price lambda;
//     4. admits a missed object only if its own predicted density beats lambda,
//        the knapsack dual condition, applied before the object ever enters the cache.
#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

#include "policy.hpp"

namespace lab {

struct ShadeParams {
    bool admission = true;      // shadow-price admission control
    bool exchange = true;       // admission by exact exchange test against the chosen victims
    bool useGap = true;         // condition on gap between last two requests
    bool useCount = true;       // condition on request count
    bool useSize = false;       // condition on object size (off: no gain on the dev slice)
    bool indexRank = true;      // rank by best density over any holding time (no horizon parameter)
    bool kaplanMeier = true;    // count objects still waiting as right-censored at their age
    double shrink = 8.0;        // empirical-Bayes pseudo-observations toward the parent hazard
    double horizonFactor = 4.0; // only if !indexRank: horizon H = factor * EWMA eviction age
    double admitSlack = 2.0;    // exchange margin: new object must be worth 2x what it displaces (tuned on dev only)
    uint32_t samples = 32;      // fresh eviction candidates per eviction
    uint32_t poolKeep = 32;     // lowest-ranked candidates carried over to the next eviction
    bool byteObjective = false; // rank by hits per request instead of hits per byte (byte hit ratio)
    uint32_t historyLog2 = 23;  // reuse statistics window: 2^historyLog2 requests
    double halfLifeLog2 = 22;   // statistics half-life: 2^halfLifeLog2 requests
    std::string label = "SHADE";
};

class ShadePolicy : public Policy {
public:
    ShadePolicy(const PolicyContext& c, ShadeParams p)
        : p_(std::move(p)), cap_(c.capacity), rng_(c.seed), set_(c.trace->numObjects),
          lastT_(c.trace->numObjects, kNever), count_(c.trace->numObjects, 0),
          cls_(c.trace->numObjects, 0), sz_(c.trace->numObjects, 0), inPool_(c.trace->numObjects, 0),
          chosen_(c.trace->numObjects, 0) {
        historyWindow_ = 1ull << p_.historyLog2;
        halfLife_ = std::exp2(p_.halfLifeLog2);
        fitDecay_ = std::exp2(-static_cast<double>(kFitInterval) / halfLife_);
        reuse_.assign(kClasses * kAgeBuckets, 0.0);
        censored_.assign(kClasses, 0.0);
        density_.assign(kClasses * kAgeBuckets, 0.0f);
        // Before the first model fit: prefer recently used, frequently used objects.
        for (uint32_t ci = 0; ci < kClasses; ++ci)
            for (uint32_t b = 0; b < kAgeBuckets; ++b)
                density_[ci * kAgeBuckets + b] = static_cast<float>((1 + countBucketOf(ci)) / (1.0 + bucketMid(b)));
    }
    std::string name() const override { return p_.label; }
    // Debug: learned density for (count, gap-bucket) at an age, size bucket 0.
    double debugDensity(uint32_t count, uint32_t gapBucket, uint32_t age) const {
        const uint32_t ci = ((p_.useCount ? countBucket(count) : 0) * kGapBuckets + gapBucket) * kSizeBuckets;
        return density_[ci * kAgeBuckets + ageBucket(age)];
    }
    uint64_t usedBytes() const override { return used_; }

    bool access(std::size_t i, uint32_t x, uint32_t size) override {
        const uint32_t now = static_cast<uint32_t>(i);
        expireHistory(now);
        // --- learn: resolve the previous request of x (a reuse at age now - lastT)
        if (lastT_[x] != kNever) {
            const uint32_t age = now - lastT_[x];
            // weight by interval START time (consistent with censored/in-flight intervals)
            if (age < historyWindow_) reuse_[cls_[x] * kAgeBuckets + ageBucket(age)] += startWeight(age);
            // else: it was already counted as censored by expireHistory
        }
        const uint32_t gap = lastT_[x] == kNever ? kNever : now - lastT_[x];
        if (count_[x] < 65535) ++count_[x];
        cls_[x] = static_cast<uint16_t>(classOf(count_[x], gap, size));
        lastT_[x] = now;
        history_.push_back({now, x});
        lastNow_ = now;
        if (++sinceFit_ >= kFitInterval) fit();

        // --- cache
        if (set_.contains(x)) return true;
        if (size > cap_) return false;
        sz_[x] = size;
        if (used_ + size > cap_) {
            if (p_.admission && fitted_ && p_.exchange) {
                if (!makeRoomByExchange(x, size, now)) return false;
            } else {
                if (p_.admission && fitted_ && rank(x, now) < p_.admitSlack * lambda_) return false;
                while (used_ + size > cap_) {
                    auto [v, r] = selectVictim(now);
                    evict(v, r, now);
                }
            }
        }
        set_.insert(x);
        used_ += size;
        return false;
    }

private:
    static constexpr uint32_t kCountBuckets = 8;
    static constexpr uint32_t kGapBuckets = 26;  // 0 = no previous request, 1..25 = log2(gap)+1
    static constexpr uint32_t kSizeBuckets = 12; // log4(size/64): 64 B .. 256 MB
    static constexpr uint32_t kParents = kCountBuckets * kGapBuckets;  // (count, gap)
    static constexpr uint32_t kClasses = kParents * kSizeBuckets;       // (count, gap, size)
    static constexpr uint32_t kAgeBuckets = 104; // 4 per octave, ages up to 2^26
    static constexpr uint32_t kFitInterval = 1u << 18;

    struct Ev { uint32_t t, id; };

    static uint32_t countBucket(uint32_t k) {
        static const uint32_t edges[kCountBuckets] = {1, 2, 3, 4, 6, 9, 15, 0xFFFFFFFF};
        for (uint32_t b = 0; b < kCountBuckets; ++b)
            if (k <= edges[b]) return b;
        return kCountBuckets - 1;
    }
    static uint32_t countBucketOf(uint32_t ci) { return ci / (kGapBuckets * kSizeBuckets); }
    static uint32_t parentOf(uint32_t ci) { return ci / kSizeBuckets; }
    uint32_t sizeBucket(uint32_t size) const {
        if (!p_.useSize) return 0;
        uint32_t lg = 0;
        while ((1u << lg) < size && lg < 31) ++lg;
        const int b = (static_cast<int>(lg) - 6) / 2;
        return static_cast<uint32_t>(std::min<int>(kSizeBuckets - 1, std::max(0, b)));
    }
    uint32_t classOf(uint32_t k, uint32_t gap, uint32_t size) const {
        const uint32_t kb = p_.useCount ? countBucket(k) : 0;
        uint32_t gb = 0;
        if (p_.useGap && gap != kNever) {
            gb = 1;
            uint32_t g = gap;
            while (g > 1 && gb < kGapBuckets - 1) { g >>= 1; ++gb; }
        }
        return (kb * kGapBuckets + gb) * kSizeBuckets + sizeBucket(size);
    }
    static uint32_t ageBucket(uint32_t age) {
        if (age == 0) return 0;
        const double l = std::log2(static_cast<double>(age)) * 4.0;
        const uint32_t b = static_cast<uint32_t>(l) + 1;
        return std::min(b, kAgeBuckets - 1);
    }
    static double bucketMid(uint32_t b) {
        if (b == 0) return 0.5;
        return std::exp2((b - 1 + 0.5) / 4.0);
    }

    void expireHistory(uint32_t now) {
        while (!history_.empty() && now - history_.front().t >= historyWindow_) {
            const Ev e = history_.front();
            history_.pop_front();
            if (lastT_[e.id] == e.t) censored_[cls_[e.id]] += startWeight(now - e.t);  // not reused within the window
        }
    }

    // Hit density for every (class, age bucket): expected hits before the horizon divided by
    // expected remaining residency (requests), for an object that has survived to that age.
    void fit() {
        sinceFit_ = 0;
        if (p_.indexRank) fitIndex();
        else fitHorizon();
        for (uint32_t ci = 0; ci < kClasses; ++ci) {
            for (uint32_t b = 0; b < kAgeBuckets; ++b) reuse_[ci * kAgeBuckets + b] *= fitDecay_;
            censored_[ci] *= fitDecay_;
        }
        fitted_ = true;
    }

    static double bucketHi(uint32_t b) { return b == 0 ? 1.0 : std::exp2(b / 4.0); }

    // Index rank (default): for an object of class c that has survived to age a, the best
    // achievable hits per unit of residency over ANY future holding time tau:
    //   I(c, a) = max_tau  P(a < D <= tau | D > a) / E[min(D, tau) - a | D > a]
    // This is the optimal-stopping (Gittins-type) form of the knapsack value/weight ratio;
    // keeping an object is worthwhile iff I/size exceeds the shadow price lambda.
    void fitIndex() {
        // Right-censoring (Kaplan-Meier): an object still waiting for its next request is known
        // to have survived to its current age. Without this, long reuse times are under-counted.
        std::vector<double> inflight(kClasses * kAgeBuckets, 0.0);
        if (p_.kaplanMeier) {
            const uint32_t now = lastNow_;
            for (const Ev& e : history_)
                if (lastT_[e.id] == e.t) inflight[cls_[e.id] * kAgeBuckets + ageBucket(now - e.t)] += startWeight(now - e.t);
        }
        const uint32_t windowBucket = ageBucket(static_cast<uint32_t>(std::min<uint64_t>(historyWindow_, 0xFFFFFFFFu)));
        (void)windowBucket;
        // at-risk(b) = everything observed to reach bucket b: reuses at >= b, censored at >= b
        std::vector<double> risk(kClasses * kAgeBuckets);
        for (uint32_t ci = 0; ci < kClasses; ++ci) {
            double atRisk = censored_[ci];
            for (uint32_t b = kAgeBuckets; b-- > 0;) {
                atRisk += reuse_[ci * kAgeBuckets + b] + inflight[ci * kAgeBuckets + b];
                risk[ci * kAgeBuckets + b] = atRisk;
            }
        }
        // Empirical-Bayes shrinkage: a sparse (count, gap, size) cell borrows strength from its
        // parent (count, gap), which borrows from its count bucket, which borrows from the global
        // hazard. m = pseudo-observations at each level.
        const double m = p_.shrink;
        std::vector<double> gR(kAgeBuckets, 0), gRisk(kAgeBuckets, 0);
        std::vector<double> cR(kCountBuckets * kAgeBuckets, 0), cRisk(kCountBuckets * kAgeBuckets, 0);
        std::vector<double> pR(kParents * kAgeBuckets, 0), pRisk(kParents * kAgeBuckets, 0);
        for (uint32_t ci = 0; ci < kClasses; ++ci)
            for (uint32_t b = 0; b < kAgeBuckets; ++b) {
                const double r = reuse_[ci * kAgeBuckets + b], k = risk[ci * kAgeBuckets + b];
                gR[b] += r; gRisk[b] += k;
                cR[countBucketOf(ci) * kAgeBuckets + b] += r;
                cRisk[countBucketOf(ci) * kAgeBuckets + b] += k;
                pR[parentOf(ci) * kAgeBuckets + b] += r;
                pRisk[parentOf(ci) * kAgeBuckets + b] += k;
            }
        std::vector<double> hazard(kAgeBuckets);
        for (uint32_t ci = 0; ci < kClasses; ++ci) {
            const double* R = &reuse_[ci * kAgeBuckets];
            const uint32_t cc = countBucketOf(ci), pc = parentOf(ci);
            for (uint32_t b = 0; b < kAgeBuckets; ++b) {
                const double hg = gRisk[b] > 1e-9 ? gR[b] / gRisk[b] : 0.0;
                const double hc = (cR[cc * kAgeBuckets + b] + m * hg) / (cRisk[cc * kAgeBuckets + b] + m);
                const double hp = (pR[pc * kAgeBuckets + b] + m * hc) / (pRisk[pc * kAgeBuckets + b] + m);
                hazard[b] = (R[b] + m * hp) / (risk[ci * kAgeBuckets + b] + m);
            }
            for (uint32_t s = 0; s < kAgeBuckets; ++s) {
                const double a = s == 0 ? 0.0 : std::exp2((s - 1) / 4.0);
                double surv = 1.0, pHit = 0, lifeHit = 0;
                float best = 0.f;
                for (uint32_t e = s; e < kAgeBuckets; ++e) {
                    const double h = surv * hazard[e];
                    pHit += h;
                    lifeHit += h * std::max(0.0, bucketMid(e) - a);
                    surv -= h;
                    const double life = lifeHit + surv * (bucketHi(e) - a);
                    if (pHit > 0 && life > 0) best = std::max(best, static_cast<float>(pHit / life));
                    if (surv < 1e-6) break;
                }
                density_[ci * kAgeBuckets + s] = best;
            }
        }
    }

    void fitHorizon() {
        const double H = std::max(64.0, std::min(p_.horizonFactor * evictAge_, static_cast<double>(historyWindow_)));
        const uint32_t hb = ageBucket(static_cast<uint32_t>(std::min(H, 4e9)));
        for (uint32_t ci = 0; ci < kClasses; ++ci) {
            double* R = &reuse_[ci * kAgeBuckets];
            // Suffix sums from old to young ages.
            double hitsWithin = 0, beyond = censored_[ci], lifeWithinTimesCount = 0;
            for (uint32_t b = kAgeBuckets; b-- > 0;) {
                if (b > hb) beyond += R[b];
                else {
                    hitsWithin += R[b];
                    lifeWithinTimesCount += R[b] * bucketMid(b);
                }
                const double a = bucketMid(b) * (b == 0 ? 0.0 : 1.0);
                const double alive = hitsWithin + beyond;
                float d = 0.f;
                if (alive > 1e-9) {
                    const double pHit = hitsWithin / alive;
                    const double life = (lifeWithinTimesCount - hitsWithin * a + beyond * std::max(0.0, H - a)) / alive;
                    d = static_cast<float>(pHit / std::max(1.0, life));
                }
                density_[ci * kAgeBuckets + b] = d;
            }
        }
    }

    // Exponential forgetting by interval start time: an interval that started `age` requests ago
    // weighs 2^(-age / halfLife), whether it ended in a reuse, was censored, or is still open.
    double startWeight(uint32_t age) const { return std::exp2(-static_cast<double>(age) / halfLife_); }

    double rank(uint32_t x, uint32_t now) const {
        const uint32_t age = now - lastT_[x];
        const double d = density_[cls_[x] * kAgeBuckets + ageBucket(age)];
        return p_.byteObjective ? d : d / sz_[x];
    }

    // Sampled victim selection with a carry-over pool (as in Redis' eviction pool): the
    // lowest-ranked candidates of earlier samples stay in the pool and are re-ranked at their
    // current age, so each decision effectively compares far more than `samples` objects.
    // Objects flagged in chosen_ (already picked in this decision) are skipped.
    std::pair<uint32_t, double> selectVictim(uint32_t now) {
        const uint32_t n = static_cast<uint32_t>(set_.size());
        for (uint32_t k = 0; k < p_.samples; ++k) {
            const uint32_t y = set_.at(rng_.below(n));
            if (inPool_[y] || chosen_[y]) continue;
            pool_.push_back(y);
            inPool_[y] = 1;
        }
        uint32_t victim = kNever;
        double worst = std::numeric_limits<double>::max();
        std::size_t victimPos = 0;
        scored_.clear();
        for (const uint32_t y : pool_) {
            const double r = rank(y, now);
            scored_.push_back({r, y});
            if (r < worst) { worst = r; victim = y; victimPos = scored_.size() - 1; }
        }
        if (victim == kNever) return {kNever, 0.0};
        scored_.erase(scored_.begin() + static_cast<std::ptrdiff_t>(victimPos));
        const std::size_t keep = std::min<std::size_t>(p_.poolKeep, scored_.size());
        std::nth_element(scored_.begin(), scored_.begin() + static_cast<std::ptrdiff_t>(keep), scored_.end());
        for (const uint32_t y : pool_) inPool_[y] = 0;
        pool_.clear();
        for (std::size_t k = 0; k < keep; ++k) {
            pool_.push_back(scored_[k].second);
            inPool_[scored_[k].second] = 1;
        }
        return {victim, worst};
    }

    void evict(uint32_t victim, double r, uint32_t now) {
        lambda_ = 0.9 * lambda_ + 0.1 * r;
        const double age = static_cast<double>(now - lastT_[victim]);
        evictAge_ = evictAge_ <= 0 ? age : 0.999 * evictAge_ + 0.001 * age;
        set_.erase(victim);
        used_ -= sz_[victim];
    }

    // Knapsack exchange test: admit x only if its value per byte beats that of every object it
    // would displace. Returns false (and leaves the cache untouched) otherwise.
    bool makeRoomByExchange(uint32_t x, uint32_t size, uint32_t now) {
        const double mine = rank(x, now);
        victims_.clear();
        uint64_t freed = 0;
        bool ok = true;
        while (used_ - freed + size > cap_) {
            auto [v, r] = selectVictim(now);
            if (v == kNever || r * p_.admitSlack > mine) { ok = false; break; }
            victims_.push_back({v, r});
            chosen_[v] = 1;
            freed += sz_[v];
        }
        for (auto& vr : victims_) chosen_[vr.first] = 0;
        if (!ok) return false;
        for (auto& vr : victims_) evict(vr.first, vr.second, now);
        return true;
    }

    ShadeParams p_;
    uint64_t cap_, used_ = 0;
    Rng rng_;
    SampleSet set_;
    std::vector<uint32_t> lastT_;
    std::vector<uint16_t> count_;
    std::vector<uint16_t> cls_;
    std::vector<uint32_t> sz_;
    std::deque<Ev> history_;
    uint64_t historyWindow_;
    std::vector<double> reuse_, censored_;
    std::vector<float> density_;
    uint32_t sinceFit_ = 0, lastNow_ = 0;
    double halfLife_ = 1, fitDecay_ = 1;
    std::vector<uint32_t> pool_;
    std::vector<char> inPool_, chosen_;
    std::vector<std::pair<uint32_t, double>> victims_;
    std::vector<std::pair<double, uint32_t>> scored_;
    bool fitted_ = false;
    double lambda_ = 0, evictAge_ = 0;
};

}  // namespace lab
