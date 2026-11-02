#include "cdn/Strategies.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include "cdn/Knapsack.hpp"

namespace cdn {

// ============================================================== SizedLRU ==
bool SizedLRU::touch(int id) {
    auto it = pos_.find(id);
    if (it == pos_.end()) return false;
    order_.splice(order_.begin(), order_, it->second.first);
    return true;
}

bool SizedLRU::insert(int id, i64 sizeKB, std::vector<int>* evicted) {
    if (sizeKB > cap_) return false;
    if (touch(id)) return true;
    while (used_ + sizeKB > cap_ && !order_.empty()) {
        int victim = order_.back();
        if (evicted) evicted->push_back(victim);
        remove(victim);
    }
    order_.push_front(id);
    pos_[id] = {order_.begin(), sizeKB};
    used_ += sizeKB;
    return true;
}

bool SizedLRU::insertColdIfRoom(int id, i64 sizeKB) {
    if (pos_.count(id)) return true;
    if (used_ + sizeKB > cap_) return false;
    order_.push_back(id);
    pos_[id] = {std::prev(order_.end()), sizeKB};
    used_ += sizeKB;
    return true;
}

bool SizedLRU::remove(int id) {
    auto it = pos_.find(id);
    if (it == pos_.end()) return false;
    used_ -= it->second.second;
    order_.erase(it->second.first);
    pos_.erase(it);
    return true;
}

void SizedLRU::setCapacity(i64 capKB, std::vector<int>* evicted) {
    cap_ = std::max<i64>(0, capKB);
    while (used_ > cap_ && !order_.empty()) {
        int victim = order_.back();
        if (evicted) evicted->push_back(victim);
        remove(victim);
    }
}

// =================================================================== LRU ==
bool LRUCache::access(const Request& r, i64) {
    if (lru_.touch(r.objectId)) return true;
    std::vector<int> ev;
    if (lru_.insert(r.objectId, cat_[r.objectId].sizeKB, &ev)) {
        ++stats.admissions;
        stats.evictions += static_cast<i64>(ev.size());
    }
    return false;
}

// =================================================================== LFU ==
void LFUCache::evictOne() {
    auto node = freqs_.begin();
    int victim = node->items.back();  // least recent among least frequent
    node->items.pop_back();
    if (node->items.empty()) freqs_.erase(node);
    used_ -= cat_[victim].sizeKB;
    map_.erase(victim);
    ++stats.evictions;
}

bool LFUCache::access(const Request& r, i64) {
    const int id = r.objectId;
    auto found = map_.find(id);
    if (found != map_.end()) {
        Entry& e = found->second;
        auto node = e.node;
        auto nxt = std::next(node);
        if (nxt == freqs_.end() || nxt->freq != node->freq + 1)
            nxt = freqs_.insert(nxt, FreqNode{node->freq + 1, {}});
        nxt->items.splice(nxt->items.begin(), node->items, e.it);  // iterator stays valid
        e.node = nxt;
        if (node->items.empty()) freqs_.erase(node);
        return true;
    }
    const i64 sz = cat_[id].sizeKB;
    if (sz > cap_) return false;
    while (used_ + sz > cap_ && !freqs_.empty()) evictOne();
    if (freqs_.empty() || freqs_.front().freq != 1) freqs_.push_front(FreqNode{1, {}});
    freqs_.front().items.push_front(id);
    map_[id] = Entry{freqs_.begin(), freqs_.front().items.begin()};
    used_ += sz;
    ++stats.admissions;
    return false;
}

std::vector<int> LFUCache::contents() const {
    std::vector<int> v;
    v.reserve(map_.size());
    for (const auto& kv : map_) v.push_back(kv.first);
    return v;
}

// ================================================================== GDSF ==
bool GDSFCache::access(const Request& r, i64) {
    const int id = r.objectId;
    const Object& o = cat_[id];
    const double perByte = cat_.hitBenefit(o, w_) / static_cast<double>(o.sizeKB);
    auto it = meta_.find(id);
    if (it != meta_.end()) {
        heap_.erase({it->second.h, id});
        it->second.freq += 1;
        it->second.h = L_ + static_cast<double>(it->second.freq) * perByte;
        heap_.insert({it->second.h, id});
        return true;
    }
    if (o.sizeKB > cap_) return false;
    while (used_ + o.sizeKB > cap_ && !heap_.empty()) {
        auto victim = *heap_.begin();
        heap_.erase(heap_.begin());
        L_ = victim.first;  // inflation: ages everything still cached
        used_ -= cat_[victim.second].sizeKB;
        meta_.erase(victim.second);
        ++stats.evictions;
    }
    Meta m{L_ + perByte, 1};
    meta_[id] = m;
    heap_.insert({m.h, id});
    used_ += o.sizeKB;
    ++stats.admissions;
    return false;
}

std::vector<int> GDSFCache::contents() const {
    std::vector<int> v;
    for (const auto& kv : meta_) v.push_back(kv.first);
    return v;
}

// ================================================================ Belady ==
bool BeladyCache::access(const Request& r, i64 gi) {
    if (!trace_) throw std::logic_error("Belady needs bindTrace()");
    const int id = r.objectId;
    const i64 nu = trace_->nextUse[static_cast<std::size_t>(gi)];
    auto it = next_.find(id);
    if (it != next_.end()) {
        byNext_.erase({it->second, id});
        it->second = nu;
        byNext_.insert({nu, id});
        return true;
    }
    const i64 sz = cat_[id].sizeKB;
    if (nu == kInfTick || sz > cap_) return false;  // never requested again: bypass
    const i64 need = used_ + sz - cap_;
    if (need > 0) {
        std::vector<int> victims;
        i64 freed = 0;
        for (auto rit = byNext_.rbegin(); rit != byNext_.rend() && freed < need; ++rit) {
            if (rit->first <= nu) break;  // everything left is needed sooner than us
            victims.push_back(rit->second);
            freed += cat_[rit->second].sizeKB;
        }
        if (freed < need) return false;  // bypass: admitting would hurt
        for (int v : victims) {
            byNext_.erase({next_[v], v});
            next_.erase(v);
            used_ -= cat_[v].sizeKB;
            ++stats.evictions;
        }
    }
    next_[id] = nu;
    byNext_.insert({nu, id});
    used_ += sz;
    ++stats.admissions;
    return false;
}

std::vector<int> BeladyCache::contents() const {
    std::vector<int> v;
    for (const auto& kv : next_) v.push_back(kv.first);
    return v;
}

// =============================================================== Planner ==
const char* KnapsackPlannerCache::expertName(int k) {
    static const char* names[kExperts] = {"last-epoch", "ewma-slow", "ewma-fast", "damped-holt"};
    return names[k];
}

KnapsackPlannerCache::KnapsackPlannerCache(const Catalog& c, i64 cap, BenefitWeights w, PlannerConfig cfg,
                                           int epochLen)
    : ICacheStrategy(c, cap, w), cfg_(std::move(cfg)), epochLen_(epochLen),
      window_(cap) {  // cold start: until the first plan, the whole cache is an LRU
    const std::size_t n = c.size();
    count_.assign(n, 0.0);
    recent_.assign(n, 0.0);
    level_.assign(n, 0.0);
    trend_.assign(n, 0.0);
    tracked_.assign(n, 0);
    fcur_.assign(n, 0.0);
    if (cfg_.model == FreqModel::Ensemble) {
        for (auto& e : ex_) e.assign(n, 0.0);
        exTrend_.assign(n, 0.0);
    }
}

namespace {
constexpr double kSlowA = 0.25, kFastA = 0.6, kHoltA = 0.5, kHoltB = 0.3, kHoltPhi = 0.8;
}

double KnapsackPlannerCache::expertForecast(int k, std::size_t id) const {
    if (k == 3) return std::max(0.0, ex_[3][id] + kHoltPhi * exTrend_[id]);
    return ex_[k][id];
}

std::vector<double> KnapsackPlannerCache::forecast() const {
    std::vector<double> f(cat_.size(), 0.0);
    for (int id : trackedIds_) {
        const auto k = static_cast<std::size_t>(id);
        switch (cfg_.model) {
            case FreqModel::Holt:
                f[k] = std::max(0.0, level_[k] + trend_[k]);
                break;
            case FreqModel::Ensemble: {
                double v = 0;
                for (int e = 0; e < kExperts; ++e) v += hedgeW_[e] * expertForecast(e, k);
                f[k] = v;
                break;
            }
            default:
                f[k] = level_[k];
        }
    }
    return f;
}

void KnapsackPlannerCache::beginEpoch(int epoch) {
    epoch_ = epoch;
    reqInEpoch_ = 0;
    driftReplansThisEpoch_ = 0;
    intervalReq_ = 0;
    if (cfg_.model == FreqModel::Oracle) {
        if (!trace_) throw std::logic_error("oracle planner needs bindTrace()");
        std::fill(fcur_.begin(), fcur_.end(), 0.0);
        const std::size_t lo = trace_->epochStart[static_cast<std::size_t>(epoch)];
        const std::size_t hi = trace_->epochStart[static_cast<std::size_t>(epoch) + 1];
        for (std::size_t i = lo; i < hi; ++i) fcur_[static_cast<std::size_t>(trace_->requests[i].objectId)] += 1;
        replan(fcur_, "epoch", static_cast<i64>(lo));
        return;
    }
    if (!haveHistory_) return;  // nothing known yet: plan stays empty (window still works)
    fcur_ = forecast();
    const i64 start = trace_ ? static_cast<i64>(trace_->epochStart[static_cast<std::size_t>(epoch)]) : 0;
    replan(fcur_, "epoch", start);
}

/// Burst detector: under the forecast, object i should receive ~Poisson(mu_i)
/// requests in this interval. Objects with k_i > mu_i + z*sqrt(mu_i) + 2 are
/// emerging heavy hitters; if their excess traffic is a material share of the
/// interval, re-plan with their observed rate boosted into the forecast.
void KnapsackPlannerCache::checkBurst(i64 gi) {
    const i64 interval = intervalReq_;
    const double scale = static_cast<double>(interval) / static_cast<double>(epochLen_);
    double excess = 0;
    std::vector<std::pair<int, double>> hot;
    for (int id : recentTouched_) {
        const auto k = static_cast<std::size_t>(id);
        const double cnt = recent_[k];
        if (cnt < cfg_.burstMinCount) continue;
        const double mu = fcur_[k] * scale;
        if (cnt > mu + cfg_.burstZ * std::sqrt(mu) + 2.0) {
            excess += cnt - mu;
            hot.push_back({id, cnt / scale});
        }
    }
    const bool roomLeft = epochLen_ - reqInEpoch_ > interval;
    if (excess / static_cast<double>(interval) >= cfg_.burstMass && roomLeft &&
        driftReplansThisEpoch_ < cfg_.maxDriftReplans) {
        for (auto [id, rate] : hot) {
            auto k = static_cast<std::size_t>(id);
            fcur_[k] = std::max(fcur_[k], rate);
        }
        replan(fcur_, "burst", gi);
        ++driftReplansThisEpoch_;
        ++stats.driftReplans;
    }
}

bool KnapsackPlannerCache::access(const Request& r, i64 gi) {
    const int id = r.objectId;
    const auto k = static_cast<std::size_t>(id);
    if (count_[k] == 0) touched_.push_back(id);
    count_[k] += 1;
    if (recent_[k] == 0) recentTouched_.push_back(id);
    recent_[k] += 1;

    bool hit = planned_.count(id) > 0 || window_.touch(id);
    if (!hit && window_.capacity() > 0) {  // admission window (or cold-start LRU before the first plan)
        std::vector<int> ev;
        if (window_.insert(id, cat_[id].sizeKB, &ev)) {
            ++stats.admissions;
            stats.evictions += static_cast<i64>(ev.size());
        }
    }
    ++reqInEpoch_;
    ++intervalReq_;

    const i64 interval = std::max<i64>(1, epochLen_ / std::max(1, cfg_.checksPerEpoch));
    if (intervalReq_ >= interval) {
        if (cfg_.driftDetect && cfg_.model != FreqModel::Oracle && haveHistory_) checkBurst(gi);
        for (int t : recentTouched_) recent_[static_cast<std::size_t>(t)] = 0;
        recentTouched_.clear();
        intervalReq_ = 0;
    }
    return hit;
}

void KnapsackPlannerCache::endEpoch(int) {
    // 1) newly seen objects join the tracked set (all experts forecast 0 for them so far)
    for (int id : touched_) {
        auto k = static_cast<std::size_t>(id);
        if (!tracked_[k]) {
            tracked_[k] = 1;
            trackedIds_.push_back(id);
        }
    }
    // 2) Hedge: score each expert by benefit-weighted absolute forecast error
    if (cfg_.model == FreqModel::Ensemble) {
        double err[kExperts] = {0, 0, 0, 0};
        double norm = 0;
        for (int id : trackedIds_) {
            auto k = static_cast<std::size_t>(id);
            const double b = cat_.hitBenefit(cat_[id], w_);
            const double y = count_[k];
            norm += b * y;
            for (int e = 0; e < kExperts; ++e) err[e] += b * std::fabs(expertForecast(e, k) - y);
        }
        if (norm > 0) {
            double minL = 1e300;
            for (int e = 0; e < kExperts; ++e) {
                hedgeLoss_[e] = cfg_.hedgeDiscount * hedgeLoss_[e] + err[e] / norm;
                minL = std::min(minL, hedgeLoss_[e]);
            }
            double z = 0;
            for (int e = 0; e < kExperts; ++e) z += (hedgeW_[e] = std::exp(-cfg_.hedgeEta * (hedgeLoss_[e] - minL)));
            for (double& w : hedgeW_) w /= z;
        }
    }
    // 3) update model state
    std::vector<int> keep;
    keep.reserve(trackedIds_.size());
    for (int id : trackedIds_) {
        auto k = static_cast<std::size_t>(id);
        const double y = count_[k];
        double alive = y;
        switch (cfg_.model) {
            case FreqModel::LastEpoch:
            case FreqModel::Oracle:
                level_[k] = y;
                break;
            case FreqModel::Ewma:
                level_[k] = cfg_.ewmaAlpha * y + (1 - cfg_.ewmaAlpha) * level_[k];
                alive = level_[k];
                break;
            case FreqModel::Holt: {
                const double a = cfg_.holtAlpha, b = cfg_.holtBeta;
                const double L = a * y + (1 - a) * (level_[k] + trend_[k]);
                trend_[k] = b * (L - level_[k]) + (1 - b) * trend_[k];
                level_[k] = L;
                alive = level_[k] + std::max(0.0, trend_[k]);
                break;
            }
            case FreqModel::Ensemble: {
                ex_[0][k] = y;
                ex_[1][k] = kSlowA * y + (1 - kSlowA) * ex_[1][k];
                ex_[2][k] = kFastA * y + (1 - kFastA) * ex_[2][k];
                const double L = kHoltA * y + (1 - kHoltA) * (ex_[3][k] + kHoltPhi * exTrend_[k]);
                exTrend_[k] = kHoltB * (L - ex_[3][k]) + (1 - kHoltB) * kHoltPhi * exTrend_[k];
                ex_[3][k] = L;
                alive = std::max({ex_[1][k], ex_[2][k], ex_[3][k]});
                break;
            }
        }
        count_[k] = 0;
        if (alive < 0.05 && y == 0) {  // forget objects nobody asks for any more
            tracked_[k] = 0;
            level_[k] = trend_[k] = 0;
            if (cfg_.model == FreqModel::Ensemble) {
                for (auto& e : ex_) e[k] = 0;
                exTrend_[k] = 0;
            }
        } else {
            keep.push_back(id);
        }
    }
    trackedIds_.swap(keep);
    touched_.clear();
    haveHistory_ = true;
}

void KnapsackPlannerCache::replan(const std::vector<double>& freq, const std::string& reason, i64 gi) {
    Stopwatch sw;
    const i64 planCap = cfg_.windowFrac > 0
                            ? static_cast<i64>(std::floor(static_cast<double>(cap_) * (1.0 - cfg_.windowFrac)))
                            : cap_;

    // 1) candidate items with their knapsack value
    struct Cand {
        int id;
        i64 size;
        double value;
    };
    std::vector<Cand> cands;
    double totalF = 0;
    for (std::size_t k = 0; k < freq.size(); ++k) {
        if (freq[k] <= 0) continue;
        totalF += freq[k];
        const int id = static_cast<int>(k);
        const Object& o = cat_[id];
        if (o.sizeKB > planCap) continue;
        const bool cached = planned_.count(id) || window_.contains(id);
        double v = freq[k] * cat_.hitBenefit(o, w_);
        if (!cached) v -= cfg_.churnPenalty * cat_.admissionCost(o, w_);
        if (v > 0) cands.push_back({id, o.sizeKB, v});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        double l = a.value * static_cast<double>(b.size), r = b.value * static_cast<double>(a.size);
        return l != r ? l > r : a.id < b.id;
    });
    // 2) core pruning (Balas-Zemel / Pisinger): the optimum differs from the greedy
    //    order only around the break item, so drop the low-density tail.
    if (cfg_.corePruneFactor > 0) {
        i64 cum = 0;
        std::size_t cut = cands.size();
        for (std::size_t i = 0; i < cands.size(); ++i) {
            cum += cands[i].size;
            if (static_cast<double>(cum) > cfg_.corePruneFactor * static_cast<double>(planCap)) {
                cut = i + 1;
                break;
            }
        }
        cands.resize(cut);
    }

    // 3) solve. Exact item values in real KB unless the solver is a capacity-indexed DP.
    auto realItems = [&] {
        std::vector<knapsack::Item> items(cands.size());
        for (std::size_t i = 0; i < cands.size(); ++i) items[i] = {cands[i].size, cands[i].value};
        return items;
    };
    auto unitSolve = [&](const std::string& algo) {
        const i64 unit = std::max<i64>(1, (planCap + cfg_.maxUnits - 1) / cfg_.maxUnits);
        std::vector<knapsack::Item> items(cands.size());
        for (std::size_t i = 0; i < cands.size(); ++i) items[i] = {(cands[i].size + unit - 1) / unit, cands[i].value};
        return knapsack::solve(algo, items, planCap / unit);  // ceil sizes => always feasible in KB
    };
    // slack fill in exact KB (unit rounding / approximation can leave room); returns value
    auto fill = [&](const knapsack::Result& r, std::vector<char>& chosen, i64& realKB) {
        chosen.assign(cands.size(), 0);
        realKB = 0;
        double v = 0;
        for (int i : r.chosen) {
            chosen[static_cast<std::size_t>(i)] = 1;
            realKB += cands[static_cast<std::size_t>(i)].size;
            v += cands[static_cast<std::size_t>(i)].value;
        }
        for (std::size_t i = 0; i < cands.size(); ++i)
            if (!chosen[i] && realKB + cands[i].size <= planCap) {
                chosen[i] = 1;
                realKB += cands[i].size;
                v += cands[i].value;
            }
        return v;
    };
    knapsack::Result res;
    std::vector<char> chosen;
    i64 realKB = 0;
    if (cfg_.solver == "auto") {
        res = knapsack::branchAndBound(realItems(), planCap, cfg_.bnbNodeLimit);
        res.algorithm = "bnb-exact";
        if (!res.provenOptimal) {  // B&B hit its node cap: also try unit DP, keep the better plan
            const double vb = fill(res, chosen, realKB);
            knapsack::Result dp = unitSolve("rolling");
            std::vector<char> c2;
            i64 kb2 = 0;
            const double vd = fill(dp, c2, kb2);
            const double ms = res.millis + dp.millis;
            if (vd > vb) {
                res = dp;
                chosen.swap(c2);
                realKB = kb2;
            }
            res.algorithm = vd > vb ? "unitDP(bnb capped)" : "bnb(capped)";
            res.millis = ms;
        } else {
            fill(res, chosen, realKB);
        }
    } else {
        if (cfg_.solver == "rolling" || cfg_.solver == "hirschberg" || cfg_.solver == "dp2d") {
            res = unitSolve(cfg_.solver);
        } else {
            res = knapsack::solve(cfg_.solver, realItems(), planCap);
        }
        fill(res, chosen, realKB);
    }

    // 5) apply the diff against the current plan
    std::unordered_set<int> next;
    double planValue = 0, plannedF = 0;
    for (std::size_t i = 0; i < cands.size(); ++i)
        if (chosen[i]) {
            next.insert(cands[i].id);
            planValue += cands[i].value;
            plannedF += freq[static_cast<std::size_t>(cands[i].id)];
        }
    PlanEvent ev;
    ev.epoch = epoch_;
    ev.atRequest = gi;
    ev.reason = reason;
    ev.solver = res.algorithm;
    ev.candidates = static_cast<int>(cands.size());
    ev.solveMs = res.millis;
    ev.planValue = planValue;

    std::vector<int> demoted;
    for (int id : planned_)
        if (!next.count(id)) {
            demoted.push_back(id);
            if (ev.evicted.size() < 40) ev.evicted.push_back(id);
        }
    for (int id : next)
        if (!planned_.count(id)) {
            if (ev.admitted.size() < 40) ev.admitted.push_back(id);
            if (window_.contains(id)) {
                window_.remove(id);  // promotion: already at the edge, no fetch needed
            } else {
                stats.prefetchKB += cat_[id].sizeKB;
                stats.prefetchCost += cat_[id].fetchCost;
                ++stats.admissions;
            }
        }
    planned_.swap(next);
    plannedKB_ = realKB;
    std::vector<int> winEv;
    window_.setCapacity(cfg_.windowFrac > 0 ? cap_ - plannedKB_ : 0, &winEv);
    stats.evictions += static_cast<i64>(winEv.size());
    for (int id : demoted) {
        if (!(cfg_.windowFrac > 0 && window_.insertColdIfRoom(id, cat_[id].sizeKB))) ++stats.evictions;
    }
    ev.expectedHitRatio = totalF > 0 ? plannedF / totalF : 0.0;
    ev.plannedKB = plannedKB_;
    events.push_back(std::move(ev));

    ++stats.replans;
    stats.planMs += sw.ms();
    stats.plannerMemBytes = std::max(stats.plannerMemBytes, res.memoryBytes);
}

std::vector<int> KnapsackPlannerCache::contents() const {
    std::vector<int> v(planned_.begin(), planned_.end());
    for (int id : window_.ids()) v.push_back(id);
    return v;
}

// =============================================================== factory ==
std::vector<std::string> allStrategyKeys() {
    return {"lru",          "lfu",           "gdsf",          "greedy-epoch",    "dp-reactive",
            "dp-units",     "dp-ewma",       "dp-predictive", "pack",            "pack-noforecast",
            "pack-nochurn", "pack-nodrift",  "pack-window",   "pack-greedy",     "pack-fptas",
            "dp-oracle",    "belady"};
}

std::vector<std::string> defaultStrategyKeys() {
    return {"lru", "lfu", "gdsf", "greedy-epoch", "dp-reactive", "dp-predictive", "pack", "dp-oracle", "belady"};
}

namespace {
PlannerConfig packConfig() {
    PlannerConfig p;
    p.name = "PACK-DP";
    p.model = FreqModel::Ensemble;
    p.solver = "auto";
    p.churnPenalty = 2.0;  // tuned on held-out seeds 101-102 (see docs/REPORT.md)
    p.driftDetect = true;
    p.windowFrac = 0.0;    // an LRU admission window was tried and hurt (see ablation)
    // Optional tuning hook: CDN_PACK="window=0.05,churn=1,eta=4,discount=0.7,z=3,mass=0.02"
    if (const char* env = std::getenv("CDN_PACK")) {
        std::string spec(env);
        std::size_t pos = 0;
        while (pos < spec.size()) {
            std::size_t comma = spec.find(',', pos);
            std::string kv = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? spec.size() : comma + 1;
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq);
            const double v = std::stod(kv.substr(eq + 1));
            if (k == "window") p.windowFrac = v;
            else if (k == "churn") p.churnPenalty = v;
            else if (k == "eta") p.hedgeEta = v;
            else if (k == "discount") p.hedgeDiscount = v;
            else if (k == "z") p.burstZ = v;
            else if (k == "mass") p.burstMass = v;
            else if (k == "minc") p.burstMinCount = static_cast<int>(v);
            else if (k == "checks") p.checksPerEpoch = static_cast<int>(v);
        }
    }
    return p;
}
}  // namespace

std::unique_ptr<ICacheStrategy> makeStrategy(const std::string& key, const Catalog& cat, i64 cap,
                                             BenefitWeights w, int epochLen, i64 maxUnits) {
    if (key == "lru") return std::make_unique<LRUCache>(cat, cap, w);
    if (key == "lfu") return std::make_unique<LFUCache>(cat, cap, w);
    if (key == "gdsf") return std::make_unique<GDSFCache>(cat, cap, w);
    if (key == "belady") return std::make_unique<BeladyCache>(cat, cap, w);

    PlannerConfig p;
    if (key == "greedy-epoch") {
        p.name = "Greedy-Epoch";
        p.solver = "greedy";
    } else if (key == "dp-reactive") {
        p.name = "DP-Reactive";
    } else if (key == "dp-units") {
        p.name = "DP-Reactive (unit DP)";
        p.solver = "rolling";
    } else if (key == "dp-ewma") {
        p.name = "DP-EWMA";
        p.model = FreqModel::Ewma;
    } else if (key == "dp-predictive") {
        p.name = "DP-Predictive";
        p.model = FreqModel::Holt;
    } else if (key == "dp-oracle") {
        p.name = "DP-Oracle*";
        p.model = FreqModel::Oracle;
    } else if (key == "pack") {
        p = packConfig();
    } else if (key == "pack-noforecast") {
        p = packConfig();
        p.name = "PACK -forecast";
        p.model = FreqModel::LastEpoch;
    } else if (key == "pack-nochurn") {
        p = packConfig();
        p.name = "PACK -churn";
        p.churnPenalty = 0;
    } else if (key == "pack-nodrift") {
        p = packConfig();
        p.name = "PACK -drift";
        p.driftDetect = false;
    } else if (key == "pack-window") {
        p = packConfig();
        p.name = "PACK +LRU window";
        p.windowFrac = 0.10;
    } else if (key == "pack-greedy") {
        p = packConfig();
        p.name = "PACK (greedy solver)";
        p.solver = "greedy";
    } else if (key == "pack-fptas") {
        p = packConfig();
        p.name = "PACK (FPTAS solver)";
        p.solver = "fptas:0.05";
    } else {
        throw std::invalid_argument("unknown strategy: " + key);
    }
    p.maxUnits = maxUnits;
    return std::make_unique<KnapsackPlannerCache>(cat, cap, w, p, epochLen);
}

}  // namespace cdn
