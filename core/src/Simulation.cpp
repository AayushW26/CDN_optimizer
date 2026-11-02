#include "cdn/Simulation.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <thread>

namespace cdn {

std::vector<std::pair<std::string, double>> Summary::fields() const {
    return {{"hitRatio", hitRatio},
            {"byteHitRatio", byteHitRatio},
            {"benefitRatio", benefitRatio},
            {"avgLatencyMs", avgLatencyMs},
            {"originGB", originGB},
            {"prefetchGB", prefetchGB},
            {"requestedGB", requestedGB},
            {"originOffloadPct", originOffloadPct},
            {"baseCost", baseCost},
            {"originCost", originCost},
            {"costSavedPct", costSavedPct},
            {"planMs", planMs},
            {"nsPerRequest", nsPerRequest},
            {"replans", replans},
            {"driftReplans", driftReplans},
            {"admissions", admissions},
            {"evictions", evictions},
            {"maxUsedFrac", maxUsedFrac},
            {"plannerMemKB", plannerMemKB}};
}

void parallelFor(int n, const std::function<void(int)>& fn, int threads) {
    if (n <= 0) return;
    int t = threads > 0 ? threads : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    t = std::min(t, n);
    if (t == 1) {
        for (int i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    std::exception_ptr err;
    std::atomic<bool> failed{false};
    for (int k = 0; k < t; ++k) {
        pool.emplace_back([&] {
            for (int i = next++; i < n; i = next++) {
                if (failed) return;
                try {
                    fn(i);
                } catch (...) {
                    if (!failed.exchange(true)) err = std::current_exception();
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    if (err) std::rethrow_exception(err);
}

Summary SimulationEngine::summarize(const std::vector<EpochMetrics>& ep, int warmup, const StrategyStats& st,
                                    double maxUsedFrac) {
    EpochMetrics t;
    const int start = std::min<int>(std::max(0, warmup), static_cast<int>(ep.size()) - 1);
    for (std::size_t e = static_cast<std::size_t>(std::max(0, start)); e < ep.size(); ++e) {
        const auto& m = ep[e];
        t.requests += m.requests;
        t.hits += m.hits;
        t.reqKB += m.reqKB;
        t.hitKB += m.hitKB;
        t.latencySum += m.latencySum;
        t.benefitAll += m.benefitAll;
        t.benefitHit += m.benefitHit;
        t.baseCost += m.baseCost;
        t.missCost += m.missCost;
        t.prefetchKB += m.prefetchKB;
        t.prefetchCost += m.prefetchCost;
        t.planMs += m.planMs;
        t.accessMs += m.accessMs;
    }
    constexpr double GB = 1024.0 * 1024.0;
    Summary s;
    const double R = std::max<double>(1, static_cast<double>(t.requests));
    s.hitRatio = static_cast<double>(t.hits) / R;
    s.byteHitRatio = t.reqKB > 0 ? t.hitKB / t.reqKB : 0;
    s.benefitRatio = t.benefitAll > 0 ? t.benefitHit / t.benefitAll : 0;
    s.avgLatencyMs = t.latencySum / R;
    s.requestedGB = t.reqKB / GB;
    s.prefetchGB = t.prefetchKB / GB;
    s.originGB = (t.reqKB - t.hitKB + t.prefetchKB) / GB;
    s.originOffloadPct = t.reqKB > 0 ? 100.0 * (1.0 - (t.reqKB - t.hitKB + t.prefetchKB) / t.reqKB) : 0;
    s.baseCost = t.baseCost;
    s.originCost = t.missCost + t.prefetchCost;
    s.costSavedPct = t.baseCost > 0 ? 100.0 * (1.0 - s.originCost / t.baseCost) : 0;
    s.planMs = t.planMs;
    s.nsPerRequest = t.accessMs * 1e6 / R;
    s.replans = st.replans;
    s.driftReplans = st.driftReplans;
    s.admissions = static_cast<double>(st.admissions);
    s.evictions = static_cast<double>(st.evictions);
    s.maxUsedFrac = maxUsedFrac;
    s.plannerMemKB = static_cast<double>(st.plannerMemBytes) / 1024.0;
    return s;
}

AlgoRun SimulationEngine::runOne(ICacheStrategy& s, const Catalog& cat, const Trace& trace, const SimConfig& cfg) {
    AlgoRun out;
    out.name = s.name();
    out.oracle = s.isOracle();
    s.bindTrace(&trace);
    const double cap = static_cast<double>(std::max<i64>(1, s.capacityKB()));
    double maxUsed = 0;
    for (int e = 0; e < trace.epochs(); ++e) {
        EpochMetrics m;
        const StrategyStats before = s.stats;
        s.beginEpoch(e);
        const double planAfterBegin = s.stats.planMs;

        const std::size_t lo = trace.epochStart[static_cast<std::size_t>(e)];
        const std::size_t hi = trace.epochStart[static_cast<std::size_t>(e) + 1];
        Stopwatch accSw;
        for (std::size_t i = lo; i < hi; ++i) {
            const Request& r = trace.requests[i];
            const Object& o = cat[r.objectId];
            const bool hit = s.access(r, static_cast<i64>(i));
            ++m.requests;
            m.reqKB += static_cast<double>(o.sizeKB);
            m.baseCost += o.fetchCost;
            const double b = cat.hitBenefit(o, cfg.weights);
            m.benefitAll += b;
            if (hit) {
                m.benefitHit += b;
                ++m.hits;
                m.hitKB += static_cast<double>(o.sizeKB);
                m.latencySum += cfg.hitLatencyMs;
            } else {
                m.missCost += o.fetchCost;
                m.latencySum += cfg.hitLatencyMs + o.latencyMs;
            }
            const double used = static_cast<double>(s.usedKB());
            if (used > maxUsed) maxUsed = used;
        }
        const double accMs = accSw.ms();
        s.endEpoch(e);

        m.prefetchKB = static_cast<double>(s.stats.prefetchKB - before.prefetchKB);
        m.prefetchCost = s.stats.prefetchCost - before.prefetchCost;
        const double driftPlanMs = s.stats.planMs - planAfterBegin;  // re-plans inside the loop
        m.planMs = s.stats.planMs - before.planMs;
        m.replans = s.stats.replans - before.replans;
        m.accessMs = std::max(0.0, accMs - driftPlanMs);
        m.usedFrac = static_cast<double>(s.usedKB()) / cap;
        out.epochs.push_back(m);
    }
    if (maxUsed > cap + 1e-9)
        throw std::logic_error("capacity violated by " + s.name());
    out.summary = summarize(out.epochs, cfg.warmup, s.stats, maxUsed / cap);
    out.finalContents = s.contents();
    out.events = std::move(s.events);
    return out;
}

SimRun SimulationEngine::run(const SimConfig& cfg) {
    Stopwatch sw;
    SimRun run;
    run.cfg = cfg;
    auto cat = std::make_shared<Catalog>();
    Trace trace;
    if (!cfg.traceCsv.empty()) {
        trace = loadTraceCsv(cfg.traceCsv, cfg.epochLen, *cat);
    } else {
        if (!cfg.catalogCsv.empty()) {
            *cat = Catalog::fromCsv(cfg.catalogCsv);
        } else {
            CatalogConfig cc;
            cc.numObjects = cfg.numObjects;
            cc.seed = cfg.seed * 7919 + 13;
            *cat = Catalog::synthetic(cc);
        }
        trace = buildTrace(*cat, cfg.traffic, cfg.epochs, cfg.epochLen, cfg.seed);
    }
    run.catalog = cat;
    run.cfg.epochs = trace.epochs();  // a CSV trace decides its own epoch count
    run.totalRequests = static_cast<long long>(trace.requests.size());
    run.capacityKB = std::max<i64>(1, static_cast<i64>(cfg.capacityFrac * static_cast<double>(cat->totalKB)));
    run.markers = trace.markers;
    run.algos.resize(cfg.algos.size());
    parallelFor(
        static_cast<int>(cfg.algos.size()),
        [&](int i) {
            auto s = makeStrategy(cfg.algos[static_cast<std::size_t>(i)], *cat, run.capacityKB, cfg.weights,
                                  cfg.epochLen, cfg.maxUnits);
            AlgoRun r = runOne(*s, *cat, trace, cfg);
            r.key = cfg.algos[static_cast<std::size_t>(i)];
            run.algos[static_cast<std::size_t>(i)] = std::move(r);
        },
        cfg.threads);
    run.wallMs = sw.ms();
    return run;
}

void writeConfig(JsonWriter& j, const SimConfig& cfg) {
    j.key("config").beginObject();
    j.kv("pattern", cfg.traffic.pattern);
    j.kv("numObjects", cfg.numObjects);
    j.kv("epochs", cfg.epochs);
    j.kv("epochLen", cfg.epochLen);
    j.kv("capacityFrac", cfg.capacityFrac);
    j.kv("seed", static_cast<long long>(cfg.seed));
    j.kv("alpha", cfg.weights.alpha);
    j.kv("beta", cfg.weights.beta);
    j.kv("hitLatencyMs", cfg.hitLatencyMs);
    j.kv("warmup", cfg.warmup);
    j.kv("zipfS", cfg.traffic.zipfS);
    j.kv("flashIntensity", cfg.traffic.flashIntensity);
    j.kv("swapRate", cfg.traffic.swapRate);
    j.kv("risersPerEpoch", cfg.traffic.risersPerEpoch);
    j.kv("scanFrac", cfg.traffic.scanFrac);
    j.kv("maxUnits", static_cast<long long>(cfg.maxUnits));
    j.endObject();
}

static void writeSummary(JsonWriter& j, const Summary& s) {
    j.key("summary").beginObject();
    for (const auto& [k, v] : s.fields()) j.kv(k, v);
    j.endObject();
}

void writeRun(JsonWriter& j, const SimRun& run, bool detail) {
    const Catalog& cat = *run.catalog;
    j.beginObject();
    writeConfig(j, run.cfg);
    j.kv("capacityKB", static_cast<long long>(run.capacityKB));
    j.kv("totalRequests", run.totalRequests);
    j.kv("wallMs", run.wallMs);
    j.key("catalog").beginObject();
    j.kv("objects", static_cast<long long>(cat.size()));
    j.kv("totalGB", static_cast<double>(cat.totalKB) / (1024.0 * 1024.0));
    {
        long long kinds[3] = {0, 0, 0};
        for (const auto& o : cat.objects) kinds[std::clamp(o.kind, 0, 2)]++;
        j.kv("static", kinds[0]).kv("video", kinds[1]).kv("large", kinds[2]);
    }
    j.endObject();
    j.key("markers").beginArray();
    for (const auto& m : run.markers) {
        if (m.epoch >= run.cfg.epochs) continue;
        j.beginObject().kv("epoch", m.epoch).kv("start", m.start).kv("type", m.type).kv("label", m.label).endObject();
    }
    j.endArray();
    j.key("algorithms").beginArray();
    for (const AlgoRun& a : run.algos) {
        j.beginObject();
        j.kv("key", a.key).kv("name", a.name).kv("oracle", a.oracle);
        writeSummary(j, a.summary);
        if (detail) {
            j.key("epochs").beginObject();
            std::vector<double> hr, bhr, ben, lat, origin, pre, plan, used;
            std::vector<int> rep;
            for (const auto& m : a.epochs) {
                const double R = std::max<double>(1, static_cast<double>(m.requests));
                hr.push_back(static_cast<double>(m.hits) / R);
                bhr.push_back(m.reqKB > 0 ? m.hitKB / m.reqKB : 0);
                ben.push_back(m.benefitAll > 0 ? m.benefitHit / m.benefitAll : 0);
                lat.push_back(m.latencySum / R);
                origin.push_back((m.reqKB - m.hitKB + m.prefetchKB) / 1024.0);
                pre.push_back(m.prefetchKB / 1024.0);
                plan.push_back(m.planMs);
                used.push_back(m.usedFrac);
                rep.push_back(m.replans);
            }
            j.array("hitRatio", hr).array("byteHitRatio", bhr).array("benefitRatio", ben).array("latencyMs", lat);
            j.array("originMB", origin).array("prefetchMB", pre).array("planMs", plan);
            j.array("usedFrac", used).array("replans", rep);
            j.endObject();

            // final cache contents (largest first, capped for file size)
            std::vector<int> fc = a.finalContents;
            std::sort(fc.begin(), fc.end(), [&](int x, int y) { return cat[x].sizeKB > cat[y].sizeKB; });
            if (fc.size() > 800) fc.resize(800);
            j.key("finalContents").beginArray();
            for (int id : fc) {
                const Object& o = cat[id];
                j.beginArray().value(id).value(static_cast<long long>(o.sizeKB)).value(o.kind).value(o.originTier).endArray();
            }
            j.endArray();
            j.kv("finalContentsTotal", static_cast<long long>(a.finalContents.size()));
            {  // composition of the final cache by content kind (over all objects, not the capped list)
                double cnt[3] = {0, 0, 0}, kb[3] = {0, 0, 0};
                for (int id : a.finalContents) {
                    const int k = std::clamp(cat[id].kind, 0, 2);
                    cnt[k] += 1;
                    kb[k] += static_cast<double>(cat[id].sizeKB);
                }
                j.key("composition").beginObject();
                j.array("count", std::vector<double>(cnt, cnt + 3)).array("kb", std::vector<double>(kb, kb + 3));
                j.endObject();
            }

            j.key("events").beginArray();
            const std::size_t maxEv = 120;
            for (std::size_t k = 0; k < a.events.size() && k < maxEv; ++k) {
                const PlanEvent& ev = a.events[k];
                j.beginObject();
                j.kv("epoch", ev.epoch).kv("at", static_cast<long long>(ev.atRequest)).kv("reason", ev.reason);
                j.kv("solver", ev.solver).kv("candidates", ev.candidates).kv("solveMs", ev.solveMs);
                j.kv("planValue", ev.planValue).kv("expectedHitRatio", ev.expectedHitRatio);
                j.kv("plannedMB", static_cast<double>(ev.plannedKB) / 1024.0);
                j.array("admitted", ev.admitted).array("evicted", ev.evicted);
                j.endObject();
            }
            j.endArray();
        }
        j.endObject();
    }
    j.endArray();
    j.endObject();
}

}  // namespace cdn
