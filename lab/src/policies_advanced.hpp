// State-of-the-art baselines: LHD, AdaptSize, W-TinyLFU; offline reference Belady-Size.
#pragma once

#include <cmath>
#include <limits>
#include <unordered_map>

#include "policy.hpp"

namespace lab {

// ---------------------------------------------------------------- LHD (Beckmann et al., NSDI'18)
// Port of the authors' reference implementation (MIT licence, (C) 2017-2018 CMU), as
// shipped in libCacheSim: rank = hit density (hits per byte per unit time) learned from
// per-class histograms of hit and eviction ages, classes by recent hit ages,
// 32 random candidates + last 8 admissions, explorers during warm-up, EWMA reconfiguration
// every 2^20 accesses, automatic age coarsening.
class LhdPolicy : public Policy {
public:
    explicit LhdPolicy(const PolicyContext& c)
        : cap_(c.capacity), rng_(c.seed), idx_(c.trace->numObjects, kNever),
          recentlyAdmitted_(kAdmissions, kNever) {
        explorerBudget_ = static_cast<int64_t>(cap_ * kExplorerBudgetFraction);
        classes_.resize(kNumClasses);
        for (uint32_t ci = 0; ci < kNumClasses; ++ci) {
            auto& cl = classes_[ci];
            cl.hits.assign(kMaxAge, 0);
            cl.evictions.assign(kMaxAge, 0);
            cl.hitDensities.assign(kMaxAge, 0);
            for (uint32_t a = 0; a < kMaxAge; ++a) cl.hitDensities[a] = 1.f * (ci + 1) / (a + 1);
        }
    }
    std::string name() const override { return "LHD"; }
    uint64_t usedBytes() const override { return used_; }

    bool access(std::size_t, uint32_t x, uint32_t size) override {
        if (idx_[x] != kNever) {
            update(x, size, false);
            return true;
        }
        if (size > cap_) return false;
        while (used_ + size > cap_) {
            const uint32_t v = rank();
            replaced(v);
        }
        used_ += size;
        update(x, size, true);
        return false;
    }

private:
    using rank_t = float;
    struct Tag {
        uint64_t timestamp;
        uint64_t lastHitAge, lastLastHitAge;
        uint32_t id;
        rank_t size;
        bool explorer;
    };
    struct Class {
        std::vector<rank_t> hits, evictions, hitDensities;
        rank_t totalHits = 0, totalEvictions = 0;
    };
    static constexpr uint32_t kAssociativity = 32;
    static constexpr uint32_t kAdmissions = 8;
    static constexpr rank_t kExplorerBudgetFraction = 0.01f;
    static constexpr uint32_t kExploreInverseProbability = 32;
    static constexpr uint32_t kHitAgeClasses = 16;
    static constexpr uint32_t kAppClasses = 16;
    static constexpr uint32_t kNumClasses = kHitAgeClasses * kAppClasses;
    static constexpr rank_t kAgeCoarseningErrorTolerance = 0.01f;
    static constexpr uint64_t kMaxAge = 20000;
    static constexpr uint64_t kAccsPerReconfiguration = 1 << 20;
    static constexpr rank_t kEwmaDecay = 0.9f;

    uint32_t hitAgeClass(uint64_t age) const {
        if (age == 0) return kHitAgeClasses - 1;
        uint32_t log = 0;
        while (age < kMaxAge && log < kHitAgeClasses - 1) {
            age <<= 1;
            log += 1;
        }
        return log;
    }
    Class& cls(const Tag& t) { return classes_[hitAgeClass(t.lastHitAge + t.lastLastHitAge)]; }  // app 0
    uint64_t getAge(const Tag& t) {
        const uint64_t age = (timestamp_ - t.timestamp) >> ageCoarseningShift_;
        if (age >= kMaxAge) {
            ++overflows_;
            return kMaxAge - 1;
        }
        return age;
    }
    rank_t hitDensity(const Tag& t) {
        const uint64_t age = getAge(t);
        if (age == kMaxAge - 1) return std::numeric_limits<rank_t>::lowest();
        rank_t d = cls(t).hitDensities[age] / t.size;
        if (t.explorer) d += 1.f;
        return d;
    }
    uint32_t rank() {
        uint64_t victim = kNever;
        rank_t victimRank = std::numeric_limits<rank_t>::max();
        const uint32_t candidates = numReconfigurations_ > 50 ? kAssociativity : 8;
        for (uint32_t i = 0; i < candidates; ++i) {
            const uint64_t k = rng_.next() % tags_.size();
            const rank_t r = hitDensity(tags_[k]);
            if (r < victimRank) { victim = k; victimRank = r; }
        }
        for (uint32_t i = 0; i < kAdmissions; ++i) {
            const uint32_t id = recentlyAdmitted_[i];
            if (id == kNever || idx_[id] == kNever) continue;
            const uint64_t k = idx_[id];
            const rank_t r = hitDensity(tags_[k]);
            if (r < victimRank) { victim = k; victimRank = r; }
        }
        ewmaVictimHitDensity_ = kEwmaDecay * ewmaVictimHitDensity_ + (1 - kEwmaDecay) * victimRank;
        return tags_[victim].id;
    }
    void update(uint32_t id, uint32_t size, bool insert) {
        Tag* tag;
        if (insert) {
            tags_.push_back(Tag{});
            tag = &tags_.back();
            idx_[id] = static_cast<uint32_t>(tags_.size() - 1);
            tag->lastLastHitAge = kMaxAge;
            tag->lastHitAge = 0;
            tag->id = id;
        } else {
            tag = &tags_[idx_[id]];
            const uint64_t age = getAge(*tag);
            cls(*tag).hits[age] += 1;
            if (tag->explorer) explorerBudget_ += static_cast<int64_t>(tag->size);
            tag->lastLastHitAge = tag->lastHitAge;
            tag->lastHitAge = age;
        }
        tag->timestamp = timestamp_;
        tag->size = static_cast<rank_t>(size);
        const bool explore = (rng_.next() % kExploreInverseProbability) == 0;
        if (explore && explorerBudget_ > 0 && numReconfigurations_ < 50) {
            tag->explorer = true;
            explorerBudget_ -= static_cast<int64_t>(tag->size);
        } else {
            tag->explorer = false;
        }
        if (insert && !explore && hitDensity(*tag) < ewmaVictimHitDensity_)
            recentlyAdmitted_[recentlyAdmittedHead_++ % kAdmissions] = id;
        ++timestamp_;
        if (--nextReconfiguration_ == 0) {
            reconfigure();
            nextReconfiguration_ = kAccsPerReconfiguration;
            ++numReconfigurations_;
        }
    }
    void replaced(uint32_t id) {
        const uint32_t k = idx_[id];
        Tag& tag = tags_[k];
        cls(tag).evictions[getAge(tag)] += 1;
        if (tag.explorer) explorerBudget_ += static_cast<int64_t>(tag.size);
        used_ -= static_cast<uint64_t>(tag.size);
        idx_[id] = kNever;
        tags_[k] = tags_.back();
        tags_.pop_back();
        if (k < tags_.size()) idx_[tags_[k].id] = k;
    }
    void reconfigure() {
        for (auto& cl : classes_) {
            cl.totalHits = cl.totalEvictions = 0;
            for (uint64_t a = 0; a < kMaxAge; ++a) {
                cl.hits[a] *= kEwmaDecay;
                cl.evictions[a] *= kEwmaDecay;
                cl.totalHits += cl.hits[a];
                cl.totalEvictions += cl.evictions[a];
            }
        }
        adaptAgeCoarsening();
        for (auto& cl : classes_) {
            rank_t totalEvents = cl.hits[kMaxAge - 1] + cl.evictions[kMaxAge - 1];
            rank_t totalHits = cl.hits[kMaxAge - 1];
            rank_t lifetimeUnconditioned = totalEvents;
            for (uint64_t a = kMaxAge - 2; a < kMaxAge; a--) {
                totalHits += cl.hits[a];
                totalEvents += cl.hits[a] + cl.evictions[a];
                lifetimeUnconditioned += totalEvents;
                cl.hitDensities[a] = totalEvents > 1e-5f ? totalHits / lifetimeUnconditioned : 0.f;
            }
        }
        overflows_ = 0;
    }
    void adaptAgeCoarsening() {
        ewmaNumObjects_ *= kEwmaDecay;
        ewmaNumObjectsMass_ *= kEwmaDecay;
        ewmaNumObjects_ += static_cast<rank_t>(tags_.size());
        ewmaNumObjectsMass_ += 1.f;
        const rank_t numObjects = ewmaNumObjects_ / ewmaNumObjectsMass_;
        const rank_t optimal = numObjects / (kAgeCoarseningErrorTolerance * kMaxAge);
        if (numReconfigurations_ == 5 || numReconfigurations_ == 25) {
            uint32_t lg = 1;
            while ((1u << lg) < optimal) lg += 1;
            const int32_t delta = static_cast<int32_t>(lg) - static_cast<int32_t>(ageCoarseningShift_);
            ageCoarseningShift_ = lg;
            ewmaNumObjects_ *= 8;
            ewmaNumObjectsMass_ *= 8;
            if (delta < 0) {
                for (auto& cl : classes_) {
                    for (uint64_t a = kMaxAge >> (-delta); a < kMaxAge - 1; a++) {
                        cl.hits[kMaxAge - 1] += cl.hits[a];
                        cl.evictions[kMaxAge - 1] += cl.evictions[a];
                    }
                    for (uint64_t a = kMaxAge - 2; a < kMaxAge; a--) {
                        cl.hits[a] = cl.hits[a >> (-delta)] / (1 << (-delta));
                        cl.evictions[a] = cl.evictions[a >> (-delta)] / (1 << (-delta));
                    }
                }
            } else if (delta > 0) {
                for (auto& cl : classes_) {
                    for (uint64_t a = 0; a < (kMaxAge >> delta); a++) {
                        cl.hits[a] = cl.hits[a << delta];
                        cl.evictions[a] = cl.evictions[a << delta];
                        for (int i = 1; i < (1 << delta); i++) {
                            cl.hits[a] += cl.hits[(a << delta) + i];
                            cl.evictions[a] += cl.evictions[(a << delta) + i];
                        }
                    }
                    for (uint64_t a = (kMaxAge >> delta); a < kMaxAge - 1; a++) {
                        cl.hits[a] = 0;
                        cl.evictions[a] = 0;
                    }
                }
            }
        }
    }

    uint64_t cap_, used_ = 0;
    Rng rng_;
    std::vector<uint32_t> idx_;
    std::vector<Tag> tags_;
    std::vector<Class> classes_;
    uint64_t timestamp_ = 0, nextReconfiguration_ = kAccsPerReconfiguration;
    int numReconfigurations_ = 0;
    uint64_t ageCoarseningShift_ = 10;
    rank_t ewmaNumObjects_ = 0, ewmaNumObjectsMass_ = 0;
    uint64_t overflows_ = 0;
    std::vector<uint32_t> recentlyAdmitted_;
    uint32_t recentlyAdmittedHead_ = 0;
    rank_t ewmaVictimHitDensity_ = 0;
    int64_t explorerBudget_ = 0;
};

// ---------------------------------------------------------------- AdaptSize (Berger et al., NSDI'17)
// LRU with probabilistic size-aware admission P(admit) = exp(-size / c). The parameter c is
// re-tuned every 30000 requests by golden-section search over the authors' Markov model of
// LRU hit ratio (port of the reference model as shipped in libCacheSim).
class AdaptSizePolicy : public Policy {
public:
    explicit AdaptSizePolicy(const PolicyContext& c)
        : cap_(c.capacity), lru_(c.trace->numObjects), in_(c.trace->numObjects, 0),
          sz_(c.trace->numObjects, 0), rng_(c.seed) {}
    std::string name() const override { return "AdaptSize"; }
    uint64_t usedBytes() const override { return used_; }

    bool access(std::size_t, uint32_t x, uint32_t size) override {
        updateStats(x, size);
        if (in_[x]) {
            lru_.moveToHead(x);
            return true;
        }
        if (size > cap_) return false;
        const double prob = std::exp(-static_cast<double>(size) / cParam_);
        if (rng_.uniform() >= prob) return false;
        while (used_ + size > cap_) {
            const uint32_t v = lru_.tail();
            lru_.remove(v);
            in_[v] = 0;
            used_ -= sz_[v];
        }
        lru_.pushHead(x);
        in_[x] = 1;
        sz_[x] = size;
        used_ += size;
        return false;
    }

private:
    struct Info {
        double seen = 0;
        int64_t size = 0;
    };
    static constexpr double kEwma = 0.3, kGssR = 0.61803399, kTol = 3.0e-8;
    static constexpr uint64_t kMaxIteration = 15, kReconfInterval = 30000;

    void updateStats(uint32_t x, uint32_t size) {
        reconfigure();
        auto iit = interval_.find(x);
        auto lit = longterm_.find(x);
        if (iit == interval_.end() && lit == longterm_.end()) {
            statSize_ += size;
        } else {
            if (iit != interval_.end() && iit->second.size != size) {
                statSize_ -= iit->second.size;
                statSize_ += size;
            }
            if (lit != longterm_.end() && lit->second.size != size) {
                statSize_ -= lit->second.size;
                statSize_ += size;
            }
        }
        auto& o = interval_[x];
        o.seen += 1.0;
        o.size = size;
    }
    void reconfigure() {
        if (--nextReconf_ > 0) return;
        if (statSize_ <= static_cast<int64_t>(cap_) * 3) {
            nextReconf_ += 1000;
            return;
        }
        nextReconf_ = kReconfInterval;
        for (auto& o : longterm_) o.second.seen *= kEwma;
        for (auto& o : interval_) {
            auto it = longterm_.find(o.first);
            if (it == longterm_.end()) {
                longterm_[o.first] = o.second;
                continue;
            }
            it->second.seen += (1 - kEwma) * o.second.seen;
            it->second.size = o.second.size;
        }
        interval_.clear();
        alSeen_.clear();
        alSize_.clear();
        for (auto it = longterm_.begin(); it != longterm_.end();) {
            if (it->second.seen < 0.1) {
                statSize_ -= it->second.size;
                it = longterm_.erase(it);
                continue;
            }
            alSeen_.push_back(it->second.seen);
            alSize_.push_back(static_cast<double>(it->second.size));
            ++it;
        }
        double x0 = 0, x1 = std::log2(static_cast<double>(cap_)), x2 = x1, x3 = x1;
        double best = 0.0;
        for (int i = 2; i < x3; i += 4) {
            const double h = modelHitRate(i);
            if (h > best) { best = h; x1 = i; }
        }
        double h1 = best, h2 = 0.0;
        const double gssV = 1 - kGssR;
        if (x3 - x1 > x1 - x0) {
            x2 = x1 + gssV * (x3 - x1);
            h2 = modelHitRate(x2);
        } else {
            x2 = x1;
            h2 = h1;
            x1 = x0 + gssV * (x1 - x0);
            h1 = modelHitRate(x1);
        }
        uint64_t it = 0;
        while (it++ < kMaxIteration && std::fabs(x3 - x0) > kTol * (std::fabs(x1) + std::fabs(x2))) {
            if (h1 != h1 || h2 != h2) break;
            if (h2 > h1) {
                x0 = x1; x1 = x2; x2 = kGssR * x1 + gssV * x3; h1 = h2; h2 = modelHitRate(x2);
            } else {
                x3 = x2; x2 = x1; x1 = kGssR * x2 + gssV * x0; h2 = h1; h1 = modelHitRate(x1);
            }
        }
        if (!(h1 != h1 || h2 != h2)) cParam_ = std::pow(2, h1 > h2 ? x1 : x2);
    }
    static double oP1(double T, double l, double p) {
        return l * p * T * (840.0 + 60.0 * l * T + 20.0 * l * l * T * T + l * l * l * T * T * T);
    }
    static double oP2(double T, double l, double p) {
        return 840.0 + 120.0 * l * (-3.0 + 7.0 * p) * T + 60.0 * l * l * (1.0 + p) * T * T +
               4.0 * l * l * l * (-1.0 + 5.0 * p) * T * T * T + l * l * l * l * p * T * T * T * T;
    }
    double modelHitRate(double log2c) {
        const double c = std::pow(2.0, log2c);
        double sum = 0.;
        for (std::size_t i = 0; i < alSeen_.size(); i++) sum += alSeen_[i] * std::exp(-alSize_[i] / c) * alSize_[i];
        if (sum <= 0) return 0;
        double T = static_cast<double>(cap_) / sum;
        alProb_.resize(alSeen_.size());
        for (std::size_t i = 0; i < alSeen_.size(); i++) alProb_[i] = std::exp(-alSize_[i] / c);
        for (int j = 0; j < 20; j++) {
            double C = 0;
            if (T > 1e70) break;
            for (std::size_t i = 0; i < alSeen_.size(); i++) {
                const double rt = alSeen_[i] * T;
                if (rt > 150) {
                    C += alSize_[i];
                } else {
                    const double e = alProb_[i] * (std::exp(rt) - 1);
                    C += alSize_[i] * e / (1 + e);
                }
            }
            T = static_cast<double>(cap_) * T / C;
        }
        double w = 0;
        for (std::size_t i = 0; i < alSeen_.size(); i++) {
            const double a = oP1(T, alSeen_[i], alProb_[i]);
            const double b = oP2(T, alSeen_[i], alProb_[i]);
            double t = (a != 0 && b == 0) ? 0.0 : a / b;
            t = std::min(1.0, std::max(0.0, t));
            w += alSeen_[i] * t;
        }
        return w;
    }

    uint64_t cap_, used_ = 0;
    IdList lru_;
    std::vector<char> in_;
    std::vector<uint32_t> sz_;
    Rng rng_;
    int64_t nextReconf_ = kReconfInterval, statSize_ = 0;
    double cParam_ = 1 << 15;
    std::unordered_map<uint32_t, Info> interval_, longterm_;
    std::vector<double> alSeen_, alSize_, alProb_;
};

// ---------------------------------------------------------------- W-TinyLFU (Einziger et al., ToS'17)
// 1% window LRU + segmented-LRU main (20% probation / 80% protected), admission by a
// 4-row count-min sketch with periodic halving. Byte-capacity generalisation: a window
// victim enters main only if its estimated frequency beats every main victim it displaces.
class WTinyLfuPolicy : public Policy {
public:
    WTinyLfuPolicy(const PolicyContext& c, uint64_t expectedObjects)
        : cap_(c.capacity), winCap_(std::max<uint64_t>(1, c.capacity / 100)),
          mainCap_(c.capacity - std::max<uint64_t>(1, c.capacity / 100)),
          protCap_(static_cast<uint64_t>((c.capacity - std::max<uint64_t>(1, c.capacity / 100)) * 0.8)),
          win_(c.trace->numObjects), prob_(c.trace->numObjects), prot_(c.trace->numObjects),
          where_(c.trace->numObjects, 0), sz_(c.trace->numObjects, 0) {
        uint64_t w = 1024;
        while (w < expectedObjects) w <<= 1;
        width_ = w;
        sketch_.assign(4 * width_, 0);
        resetAt_ = 10 * width_;
    }
    std::string name() const override { return "W-TinyLFU"; }
    uint64_t usedBytes() const override { return winUsed_ + probUsed_ + protUsed_; }

    bool access(std::size_t, uint32_t x, uint32_t size) override {
        increment(x);
        switch (where_[x]) {
            case 1: win_.moveToHead(x); return true;
            case 2:  // probation -> protected
                prob_.remove(x);
                probUsed_ -= sz_[x];
                prot_.pushHead(x);
                protUsed_ += sz_[x];
                where_[x] = 3;
                while (protUsed_ > protCap_) {
                    const uint32_t d = prot_.tail();
                    prot_.remove(d);
                    protUsed_ -= sz_[d];
                    prob_.pushHead(d);
                    probUsed_ += sz_[d];
                    where_[d] = 2;
                }
                return true;
            case 3: prot_.moveToHead(x); return true;
            default: break;
        }
        if (size > cap_) return false;
        sz_[x] = size;
        win_.pushHead(x);
        winUsed_ += size;
        where_[x] = 1;
        while (winUsed_ > winCap_) {
            const uint32_t cand = win_.tail();
            win_.remove(cand);
            winUsed_ -= sz_[cand];
            where_[cand] = 0;
            admitToMain(cand);
        }
        return false;
    }

private:
    void admitToMain(uint32_t cand) {
        const uint32_t fc = estimate(cand);
        while (probUsed_ + protUsed_ + sz_[cand] > mainCap_) {
            uint32_t v = !prob_.empty() ? prob_.tail() : prot_.tail();
            if (v == kNever) return;
            if (fc <= estimate(v)) return;  // candidate rejected
            if (where_[v] == 2) { prob_.remove(v); probUsed_ -= sz_[v]; }
            else { prot_.remove(v); protUsed_ -= sz_[v]; }
            where_[v] = 0;
        }
        prob_.pushHead(cand);
        probUsed_ += sz_[cand];
        where_[cand] = 2;
    }
    uint64_t hash(uint32_t x, int row) const {
        uint64_t z = (static_cast<uint64_t>(x) + 1) * (0x9E3779B97F4A7C15ULL + 2 * row + 1);
        z ^= z >> 29;
        z *= 0xBF58476D1CE4E5B9ULL;
        z ^= z >> 32;
        return z & (width_ - 1);
    }
    void increment(uint32_t x) {
        for (int r = 0; r < 4; ++r) {
            uint8_t& c = sketch_[r * width_ + hash(x, r)];
            if (c < 15) ++c;
        }
        if (++additions_ >= resetAt_) {
            for (auto& c : sketch_) c >>= 1;
            additions_ /= 2;
        }
    }
    uint32_t estimate(uint32_t x) const {
        uint32_t m = 255;
        for (int r = 0; r < 4; ++r) m = std::min<uint32_t>(m, sketch_[r * width_ + hash(x, r)]);
        return m;
    }
    uint64_t cap_, winCap_, mainCap_, protCap_;
    uint64_t winUsed_ = 0, probUsed_ = 0, protUsed_ = 0;
    IdList win_, prob_, prot_;
    std::vector<uint8_t> where_;  // 0 none, 1 window, 2 probation, 3 protected
    std::vector<uint32_t> sz_;
    uint64_t width_ = 1024, additions_ = 0, resetAt_ = 0;
    std::vector<uint8_t> sketch_;
};

// ---------------------------------------------------------------- Belady-Size (offline reference)
// Knows the future. Evicts, among 64 sampled cached objects, the one with the largest
// size x time-to-next-request (never requested again = infinite). Not optimal with
// variable sizes, but a strong practical reference; see the PFOO-L bound for a true bound.
class BeladySizePolicy : public Policy {
public:
    explicit BeladySizePolicy(const PolicyContext& c)
        : cap_(c.capacity), t_(c.trace), set_(c.trace->numObjects), nextUse_(c.trace->numObjects, kNever),
          sz_(c.trace->numObjects, 0), rng_(c.seed) {}
    std::string name() const override { return "Belady-Size*"; }
    uint64_t usedBytes() const override { return used_; }
    bool access(std::size_t i, uint32_t x, uint32_t size) override {
        const uint32_t nu = t_->next[i];
        if (set_.contains(x)) {
            nextUse_[x] = nu;
            return true;
        }
        if (size > cap_ || nu == kNever) return false;  // never reused: an oracle would not cache it
        nextUse_[x] = nu;
        sz_[x] = size;
        while (used_ + size > cap_) {
            uint32_t victim = kNever;
            double worst = -1;
            const std::size_t n = set_.size();
            for (int k = 0; k < 64; ++k) {
                const uint32_t y = set_.at(rng_.below(static_cast<uint32_t>(n)));
                const double d = nextUse_[y] == kNever ? 1e300 : static_cast<double>(nextUse_[y] - i) * sz_[y];
                if (d > worst) { worst = d; victim = y; }
            }
            const double mine = static_cast<double>(nu - i) * size;
            if (mine >= worst) return false;  // bypass: the new object is the worst candidate
            set_.erase(victim);
            used_ -= sz_[victim];
        }
        set_.insert(x);
        used_ += size;
        return false;
    }

private:
    uint64_t cap_, used_ = 0;
    const Trace* t_;
    SampleSet set_;
    std::vector<uint32_t> nextUse_, sz_;
    Rng rng_;
};

}  // namespace lab
