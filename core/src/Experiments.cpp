#include "cdn/Experiments.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>

#include "cdn/Json.hpp"
#include "cdn/Knapsack.hpp"
#include "cdn/Simulation.hpp"

namespace cdn {
namespace {

namespace fs = std::filesystem;
using Fields = std::vector<std::pair<std::string, double>>;

std::string writeFile(const ExperimentOptions& opt, const std::string& name, const std::string& body) {
    fs::create_directories(opt.outDir);
    const std::string path = (fs::path(opt.outDir) / name).string();
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << body << '\n';
    std::cerr << "  wrote " << path << " (" << body.size() / 1024 << " KB)\n";
    return path;
}

SimConfig baseConfig(const ExperimentOptions& opt) {
    SimConfig c;
    if (opt.quick) {
        c.numObjects = 1500;
        c.epochs = 16;
        c.epochLen = 5000;
    } else {
        c.numObjects = 5000;
        c.epochs = 40;
        c.epochLen = 15000;
    }
    c.capacityFrac = 0.05;
    c.threads = 1;  // parallelism is across runs
    return c;
}

int seedsFor(const ExperimentOptions& opt, int full) { return opt.quick ? 2 : full; }

/// Hit ratio restricted to flash-crowd onset epochs (the epoch a burst starts and the next one).
double burstHitRatio(const AlgoRun& a, const SimRun& r) {
    std::vector<char> mark(a.epochs.size(), 0);
    for (const Marker& m : r.markers) {
        if (m.type != "flash") continue;
        for (int e = m.epoch; e <= m.epoch + 1; ++e)
            if (e >= 0 && e < static_cast<int>(mark.size()) && e >= r.cfg.warmup) mark[static_cast<std::size_t>(e)] = 1;
    }
    double h = 0, q = 0;
    for (std::size_t e = 0; e < a.epochs.size(); ++e)
        if (mark[e]) {
            h += static_cast<double>(a.epochs[e].hits);
            q += static_cast<double>(a.epochs[e].requests);
        }
    return q > 0 ? h / q : std::nan("");
}

Fields algoFields(const AlgoRun& a, const SimRun& r) {
    Fields f = a.summary.fields();
    f.push_back({"burstHitRatio", burstHitRatio(a, r)});
    return f;
}

struct Agg {
    std::string key, name;
    bool oracle = false;
    std::map<std::string, std::vector<double>> samples;
    std::vector<std::string> order;

    void add(const Fields& f) {
        for (const auto& [k, v] : f) {
            if (!samples.count(k)) order.push_back(k);
            samples[k].push_back(v);
        }
    }
    static std::pair<double, double> meanStd(const std::vector<double>& xs) {
        double s = 0, n = 0;
        for (double x : xs)
            if (std::isfinite(x)) {
                s += x;
                n += 1;
            }
        if (n == 0) return {std::nan(""), std::nan("")};
        const double m = s / n;
        double var = 0;
        for (double x : xs)
            if (std::isfinite(x)) var += (x - m) * (x - m);
        return {m, n > 1 ? std::sqrt(var / (n - 1)) : 0.0};
    }
};

/// Run a list of configs in parallel; returns runs in the same order.
std::vector<SimRun> runAll(const std::vector<SimConfig>& cfgs, const ExperimentOptions& opt) {
    std::vector<SimRun> out(cfgs.size());
    std::mutex mu;
    int done = 0;
    parallelFor(
        static_cast<int>(cfgs.size()),
        [&](int i) {
            out[static_cast<std::size_t>(i)] = SimulationEngine::run(cfgs[static_cast<std::size_t>(i)]);
            std::lock_guard<std::mutex> lk(mu);
            ++done;
            if (done % std::max<int>(1, static_cast<int>(cfgs.size()) / 10) == 0 || done == static_cast<int>(cfgs.size()))
                std::cerr << "    " << done << "/" << cfgs.size() << " runs\r" << std::flush;
        },
        opt.threads);
    std::cerr << "\n";
    return out;
}

void writeAggList(JsonWriter& j, std::vector<Agg>& aggs) {
    j.key("algorithms").beginArray();
    for (auto& a : aggs) {
        j.beginObject().kv("key", a.key).kv("name", a.name).kv("oracle", a.oracle);
        j.key("mean").beginObject();
        for (const auto& k : a.order) j.kv(k, Agg::meanStd(a.samples[k]).first);
        j.endObject();
        j.key("std").beginObject();
        for (const auto& k : a.order) j.kv(k, Agg::meanStd(a.samples[k]).second);
        j.endObject();
        j.endObject();
    }
    j.endArray();
}

std::vector<Agg> aggregate(const std::vector<const SimRun*>& runs) {
    std::vector<Agg> aggs;
    if (runs.empty()) return aggs;
    for (const auto& a : runs[0]->algos) {
        Agg g;
        g.key = a.key;
        g.name = a.name;
        g.oracle = a.oracle;
        aggs.push_back(g);
    }
    for (const SimRun* r : runs)
        for (std::size_t i = 0; i < r->algos.size(); ++i) aggs[i].add(algoFields(r->algos[i], *r));
    return aggs;
}

// ----------------------------------------------------------- compare-like --
std::vector<std::string> compareLike(const std::string& expName, const std::vector<std::string>& patterns,
                                     const std::vector<std::string>& algos, int seeds, bool timelines,
                                     const ExperimentOptions& opt) {
    std::cerr << "[" << expName << "] " << patterns.size() << " patterns x " << seeds << " seeds x "
              << algos.size() << " algorithms\n";
    std::vector<SimConfig> cfgs;
    for (const auto& p : patterns)
        for (int s = 1; s <= seeds; ++s) {
            SimConfig c = baseConfig(opt);
            c.traffic = presetTraffic(p);
            c.seed = static_cast<std::uint64_t>(s);
            c.algos = algos;
            cfgs.push_back(c);
        }
    std::vector<SimRun> runs = runAll(cfgs, opt);
    std::vector<std::string> files;
    JsonWriter j;
    j.beginObject().kv("experiment", expName).kv("seeds", seeds);
    writeConfig(j, baseConfig(opt));
    j.key("patterns").beginArray();
    for (std::size_t p = 0; p < patterns.size(); ++p) {
        std::vector<const SimRun*> group;
        for (int s = 0; s < seeds; ++s) group.push_back(&runs[p * static_cast<std::size_t>(seeds) + static_cast<std::size_t>(s)]);
        auto aggs = aggregate(group);
        j.beginObject().kv("pattern", patterns[p]);
        writeAggList(j, aggs);
        // Paired comparison vs PACK-DP on identical traces: seeds differ in catalog and
        // trace, so per-seed differences are far tighter than the across-seed spread.
        std::size_t packIdx = group[0]->algos.size();
        for (std::size_t a = 0; a < group[0]->algos.size(); ++a)
            if (group[0]->algos[a].key == "pack") packIdx = a;
        if (packIdx < group[0]->algos.size()) {
            j.key("pairedVsPack").beginArray();
            for (std::size_t a = 0; a < group[0]->algos.size(); ++a) {
                std::vector<double> d;
                int wins = 0;
                for (const SimRun* r : group) {
                    const double delta = r->algos[packIdx].summary.benefitRatio - r->algos[a].summary.benefitRatio;
                    d.push_back(delta);
                    wins += delta > 0;
                }
                auto ms = Agg::meanStd(d);
                j.beginObject().kv("key", group[0]->algos[a].key).kv("meanDelta", ms.first).kv("stdDelta", ms.second);
                j.kv("packWins", wins).kv("runs", static_cast<int>(d.size())).endObject();
            }
            j.endArray();
        }
        j.endObject();
        if (timelines) {
            JsonWriter t;
            writeRun(t, *group[0], true);
            files.push_back(writeFile(opt, "timeline_" + patterns[p] + ".json", t.str()));
        }
    }
    j.endArray().endObject();
    files.push_back(writeFile(opt, expName + ".json", j.str()));
    return files;
}

// ------------------------------------------------------------------ sweep --
std::vector<std::string> sweep(const std::string& expName, const std::string& xLabel, const std::vector<double>& xs,
                               const std::vector<std::string>& patterns, const std::vector<std::string>& algos,
                               int seeds, const std::function<void(SimConfig&, double)>& apply,
                               const ExperimentOptions& opt) {
    std::cerr << "[" << expName << "] " << xs.size() << " points x " << patterns.size() << " patterns x " << seeds
              << " seeds x " << algos.size() << " algorithms\n";
    std::vector<SimConfig> cfgs;
    for (const auto& p : patterns)
        for (double x : xs)
            for (int s = 1; s <= seeds; ++s) {
                SimConfig c = baseConfig(opt);
                c.traffic = presetTraffic(p);
                c.seed = static_cast<std::uint64_t>(s);
                c.algos = algos;
                apply(c, x);
                cfgs.push_back(c);
            }
    std::vector<SimRun> runs = runAll(cfgs, opt);
    JsonWriter j;
    j.beginObject().kv("experiment", expName).kv("xLabel", xLabel).kv("seeds", seeds);
    j.array("x", xs);
    writeConfig(j, baseConfig(opt));
    j.key("patterns").beginArray();
    std::size_t idx = 0;
    for (const auto& p : patterns) {
        // aggs[x][algo]
        std::vector<std::vector<Agg>> perX;
        for (std::size_t xi = 0; xi < xs.size(); ++xi) {
            std::vector<const SimRun*> group;
            for (int s = 0; s < seeds; ++s) group.push_back(&runs[idx++]);
            perX.push_back(aggregate(group));
        }
        j.beginObject().kv("pattern", p);
        j.key("series").beginArray();
        for (std::size_t a = 0; a < perX[0].size(); ++a) {
            j.beginObject().kv("key", perX[0][a].key).kv("name", perX[0][a].name).kv("oracle", perX[0][a].oracle);
            for (const char* which : {"mean", "std"}) {
                j.key(which).beginObject();
                for (const auto& field : perX[0][a].order) {
                    j.key(field).beginArray();
                    for (std::size_t xi = 0; xi < xs.size(); ++xi) {
                        auto ms = Agg::meanStd(perX[xi][a].samples[field]);
                        j.value(std::string(which) == "mean" ? ms.first : ms.second);
                    }
                    j.endArray();
                }
                j.endObject();
            }
            j.endObject();
        }
        j.endArray().endObject();
    }
    j.endArray().endObject();
    return {writeFile(opt, expName + ".json", j.str())};
}

// ---------------------------------------------------------------- solvers --
std::vector<knapsack::Item> cacheInstance(int n, Rng& rng) {
    std::vector<knapsack::Item> v(static_cast<std::size_t>(n));
    std::vector<int> rank(static_cast<std::size_t>(n));
    std::iota(rank.begin(), rank.end(), 1);
    rng.shuffle(rank);
    for (int i = 0; i < n; ++i) {
        auto& it = v[static_cast<std::size_t>(i)];
        it.weight = std::clamp<i64>(static_cast<i64>(std::llround(rng.lognormal(std::log(8.0), 1.0))), 1, 2000);
        it.value = 1e4 / std::pow(rank[static_cast<std::size_t>(i)], 0.9) * rng.lognormal(0, 0.5) *
                   (1.0 + 0.02 * static_cast<double>(it.weight));
    }
    return v;
}

std::vector<knapsack::Item> correlatedInstance(int n, int R, Rng& rng) {
    std::vector<knapsack::Item> v(static_cast<std::size_t>(n));
    for (auto& it : v) {
        it.weight = rng.intIn(1, R);
        it.value = static_cast<double>(it.weight) + R / 10.0;  // strongly correlated (Pisinger)
    }
    return v;
}

i64 halfWeight(const std::vector<knapsack::Item>& v) {
    i64 s = 0;
    for (const auto& it : v) s += it.weight;
    return std::max<i64>(1, s / 2);
}

struct SolverSpec {
    std::string name;
    // estimated work / memory (bytes) -> skip if too big
    std::function<bool(double n, double C)> feasible;
};

double median3(const std::function<double()>& f, int reps) {
    std::vector<double> t;
    for (int r = 0; r < reps; ++r) t.push_back(f());
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

double fitSlope(const std::vector<double>& x, const std::vector<double>& y) {
    // least squares on log-log, only finite positive points
    double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!(y[i] > 0.02) || !std::isfinite(y[i])) continue;
        const double a = std::log(x[i]), b = std::log(y[i]);
        sx += a;
        sy += b;
        sxx += a * a;
        sxy += a * b;
        n += 1;
    }
    if (n < 2) return std::nan("");
    return (n * sxy - sx * sy) / (n * sxx - sx * sx);
}

void scalingBlock(JsonWriter& j, const std::string& key, const std::string& varName, const std::vector<double>& vars,
                  const std::function<std::pair<std::vector<knapsack::Item>, i64>(double, Rng&)>& gen,
                  const std::vector<SolverSpec>& specs, int reps) {
    std::cerr << "  [solvers] " << key << "\n";
    std::map<std::string, std::vector<double>> ms, mem, gap, nodes;
    std::vector<double> optV;
    for (double x : vars) {
        Rng rng(1234 + static_cast<std::uint64_t>(x));
        auto [items, C] = gen(x, rng);
        knapsack::Result ref = knapsack::branchAndBound(items, C, 30'000'000);
        double opt = ref.value;
        if (!ref.provenOptimal && static_cast<double>(items.size()) * static_cast<double>(C) < 3e9) opt = knapsack::dpRolling(items, C).value;
        optV.push_back(opt);
        for (const auto& sp : specs) {
            if (!sp.feasible(static_cast<double>(items.size()), static_cast<double>(C))) {
                ms[sp.name].push_back(std::nan(""));
                mem[sp.name].push_back(std::nan(""));
                gap[sp.name].push_back(std::nan(""));
                nodes[sp.name].push_back(std::nan(""));
                continue;
            }
            knapsack::Result last;
            double t = median3(
                [&] {
                    last = knapsack::solve(sp.name, items, C);
                    return last.millis;
                },
                reps);
            ms[sp.name].push_back(t);
            mem[sp.name].push_back(static_cast<double>(last.memoryBytes) / 1024.0);
            gap[sp.name].push_back(opt > 0 ? 100.0 * (1.0 - last.value / opt) : 0.0);
            nodes[sp.name].push_back(static_cast<double>(last.nodes));
        }
    }
    j.key(key).beginObject();
    j.kv("variable", varName);
    j.array("x", vars);
    j.key("algorithms").beginArray();
    for (const auto& sp : specs) {
        j.beginObject().kv("name", sp.name);
        j.array("ms", ms[sp.name]).array("memKB", mem[sp.name]).array("gapPct", gap[sp.name]);
        j.array("nodes", nodes[sp.name]);
        j.kv("logLogSlope", fitSlope(vars, ms[sp.name]));
        j.endObject();
    }
    j.endArray().endObject();
}

std::vector<std::string> solversExperiment(const ExperimentOptions& opt) {
    std::cerr << "[solvers] knapsack solver benchmark\n";
    const int reps = opt.quick ? 1 : 3;
    auto memOk = [](double bytes) { return bytes < 7e8; };
    std::vector<SolverSpec> specs = {
        {"dp2d", [&](double n, double C) { return memOk(n * (C + 1) * 8); }},
        {"rolling", [&](double n, double C) { return memOk(n * (C + 1) / 8) && n * C < 4e9; }},
        {"hirschberg", [](double n, double C) { return n * C < 8e9; }},
        {"bnb", [](double, double) { return true; }},
        {"fptas:0.1", [&](double n, double) { return n * n * 20 < 3e9; }},
        {"greedy", [](double, double) { return true; }},
        {"modgreedy", [](double, double) { return true; }},
    };
    JsonWriter j;
    j.beginObject().kv("experiment", "solvers");

    std::vector<double> ns = opt.quick ? std::vector<double>{250, 500, 1000, 2000}
                                       : std::vector<double>{250, 500, 1000, 2000, 4000, 8000, 16000};
    scalingBlock(j, "scalingN", "n (items), C = 8192", ns,
                 [](double n, Rng& rng) { return std::make_pair(cacheInstance(static_cast<int>(n), rng), i64{8192}); },
                 specs, reps);

    std::vector<double> cs = opt.quick ? std::vector<double>{1024, 2048, 4096, 8192}
                                       : std::vector<double>{1024, 2048, 4096, 8192, 16384, 32768, 65536};
    scalingBlock(j, "scalingC", "C (capacity units), n = 2000", cs,
                 [](double C, Rng& rng) { return std::make_pair(cacheInstance(2000, rng), static_cast<i64>(C)); }, specs,
                 reps);

    // Hard, strongly-correlated instances: B&B goes exponential, DP stays pseudo-polynomial.
    std::vector<SolverSpec> hardSpecs = {
        {"rolling", [](double n, double C) { return n * C < 3e9; }},
        {"bnb", [](double, double) { return true; }},
        {"greedy", [](double, double) { return true; }},
        {"fptas:0.1", [](double n, double) { return n * n * 20 < 3e9; }},
    };
    std::vector<double> hn = opt.quick ? std::vector<double>{20, 40, 60} : std::vector<double>{20, 30, 40, 50, 60, 80, 100, 150, 200};
    scalingBlock(j, "hardCorrelated", "n (strongly correlated, R = 1000, C = sum(w)/2)", hn,
                 [](double n, Rng& rng) {
                     auto v = correlatedInstance(static_cast<int>(n), 1000, rng);
                     return std::make_pair(v, halfWeight(v));
                 },
                 hardSpecs, reps);

    // Huge capacity: DP impossible, meet-in-the-middle shines.
    std::vector<SolverSpec> mitmSpecs = {
        {"mitm", [](double n, double) { return n <= 40; }},
        {"bnb", [](double, double) { return true; }},
        {"dp2d", [&](double n, double C) { return memOk(n * (C + 1) * 8); }},
    };
    std::vector<double> mn = opt.quick ? std::vector<double>{10, 20, 30} : std::vector<double>{10, 16, 20, 24, 28, 32, 36, 40};
    scalingBlock(j, "hugeCapacity", "n (weights up to 1e9 bytes, C = sum(w)/2)", mn,
                 [](double n, Rng& rng) {
                     std::vector<knapsack::Item> v(static_cast<std::size_t>(n));
                     for (auto& it : v) {
                         it.weight = static_cast<i64>(rng.below(1'000'000'000ULL)) + 1;
                         it.value = static_cast<double>(it.weight) * rng.uniform(0.8, 1.2);
                     }
                     return std::make_pair(v, halfWeight(v));
                 },
                 mitmSpecs, reps);

    // FPTAS: accuracy / time trade-off.
    {
        std::cerr << "  [solvers] fptasEps\n";
        Rng rng(77);
        auto items = cacheInstance(2000, rng);
        const i64 C = 8192;
        const double opt = knapsack::dpRolling(items, C).value;
        std::vector<double> eps = {0.5, 0.3, 0.2, 0.1, 0.05, 0.02, 0.01};
        if (opt == 0) throw std::logic_error("degenerate instance");
        std::vector<double> ms, gap, mem;
        for (double e : eps) {
            knapsack::Result r;
            ms.push_back(median3(
                [&] {
                    r = knapsack::fptas(items, C, e);
                    return r.millis;
                },
                reps));
            gap.push_back(100.0 * (1.0 - r.value / opt));
            mem.push_back(static_cast<double>(r.memoryBytes) / 1024.0);
        }
        Rng rng2(78);
        auto hard = correlatedInstance(60, 1000, rng2);
        const i64 Ch = halfWeight(hard);
        const double optH = knapsack::dpRolling(hard, Ch).value;
        std::vector<double> gapH;
        for (double e : eps) gapH.push_back(100.0 * (1.0 - knapsack::fptas(hard, Ch, e).value / optH));
        j.key("fptasEps").beginObject();
        j.array("eps", eps).array("ms", ms).array("gapPct", gap).array("memKB", mem).array("gapPctHard", gapH);
        j.endObject();
    }
    j.endObject();
    return {writeFile(opt, "solvers.json", j.str())};
}

// -------------------------------------------------------------------- ABR --
// Multi-bitrate video caching as a Multiple-Choice Knapsack Problem.
std::vector<std::string> abrExperiment(const ExperimentOptions& opt) {
    std::cerr << "[abr] multi-bitrate video caching (MCKP)\n";
    struct Rung {
        const char* name;
        double kbps;
    };
    const std::vector<Rung> ladder = {{"240p", 400}, {"360p", 800}, {"480p", 1400},
                                      {"720p", 2800}, {"1080p", 5000}, {"2160p", 16000}};
    const std::vector<double> deviceMix = {0.06, 0.12, 0.18, 0.30, 0.26, 0.08};
    const int Q = static_cast<int>(ladder.size());
    const int V = opt.quick ? 400 : 1500;
    const int epochs = opt.quick ? 6 : 12;
    const int perEpoch = opt.quick ? 10000 : 40000;
    const double transcodeKeep = 0.85;  // a transcoded hit keeps 85% of the benefit
    const int seeds = seedsFor(opt, 3);
    std::vector<double> capFracs = {0.01, 0.02, 0.05, 0.10, 0.20};
    const std::vector<std::string> policies = {"LRU (per version)", "Independent 0/1 KP", "Top-version KP",
                                               "MCKP (transcode-aware)"};
    // result[policy][cap] -> samples
    std::map<std::string, std::vector<std::vector<double>>> hr, bsaved, trans, cost, bene;
    for (const auto& p : policies) {
        bene[p].assign(capFracs.size(), {});
        hr[p].assign(capFracs.size(), {});
        bsaved[p].assign(capFracs.size(), {});
        trans[p].assign(capFracs.size(), {});
        cost[p].assign(capFracs.size(), {});
    }
    double mckpMs = 0;
    std::size_t mckpMem = 0;

    for (int seed = 1; seed <= seeds; ++seed) {
        Rng rng(static_cast<std::uint64_t>(500 + seed));
        // videos
        std::vector<std::vector<double>> sizeKB(static_cast<std::size_t>(V), std::vector<double>(static_cast<std::size_t>(Q)));
        std::vector<std::vector<double>> qmix(static_cast<std::size_t>(V), std::vector<double>(static_cast<std::size_t>(Q)));
        std::vector<double> price(static_cast<std::size_t>(V)), lat(static_cast<std::size_t>(V));
        double totalKB = 0, sumCost = 0, sumLat = 0;
        for (int v = 0; v < V; ++v) {
            const double minutes = std::clamp(rng.lognormal(std::log(8.0), 0.6), 1.0, 60.0);
            const int tier = static_cast<int>(rng.below(originTiers().size()));
            price[static_cast<std::size_t>(v)] = originTiers()[static_cast<std::size_t>(tier)].dollarsPerGB;
            lat[static_cast<std::size_t>(v)] = originTiers()[static_cast<std::size_t>(tier)].rttMs;
            const double tilt = rng.uniform(-1.0, 1.0);  // mobile clip (<0) ... cinema (>0)
            double z = 0;
            for (int q = 0; q < Q; ++q) {
                sizeKB[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)] = ladder[static_cast<std::size_t>(q)].kbps * minutes * 60 / 8;
                totalKB += sizeKB[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)];
                double w = deviceMix[static_cast<std::size_t>(q)] * std::exp(tilt * (q - 2.5) * 0.5);
                qmix[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)] = w;
                z += w;
            }
            for (auto& w : qmix[static_cast<std::size_t>(v)]) w /= z;
            sumCost += price[static_cast<std::size_t>(v)];
            sumLat += lat[static_cast<std::size_t>(v)];
        }
        const double meanPrice = sumCost / V, meanLat = sumLat / V;
        std::vector<int> rank(static_cast<std::size_t>(V));
        std::iota(rank.begin(), rank.end(), 0);
        rng.shuffle(rank);
        std::vector<double> pop(static_cast<std::size_t>(V));
        for (int v = 0; v < V; ++v) pop[static_cast<std::size_t>(rank[static_cast<std::size_t>(v)])] = 1.0 / std::pow(v + 1.0, 0.9);
        // per-request benefit of serving (v, q) from the edge (normalised like Catalog::hitBenefit)
        auto benefit = [&](int v, int q) {
            const double gb = sizeKB[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)] / (1024.0 * 1024.0);
            return gb * price[static_cast<std::size_t>(v)] / (meanPrice * 0.5) + lat[static_cast<std::size_t>(v)] / meanLat;
        };
        // generate epochs of (video, quality) requests
        std::vector<std::vector<std::pair<int, int>>> reqs(static_cast<std::size_t>(epochs));
        for (int e = 0; e < epochs; ++e) {
            if (e > 0)  // mild drift
                for (int s = 0; s < V / 50; ++s) std::swap(pop[rng.below(static_cast<std::uint64_t>(V))], pop[rng.below(static_cast<std::uint64_t>(V / 10))]);
            AliasSampler vs(pop);
            for (int k = 0; k < perEpoch; ++k) {
                int v = static_cast<int>(vs.sample(rng));
                double u = rng.uniform();
                int q = 0;
                while (q < Q - 1 && u > qmix[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)]) {
                    u -= qmix[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)];
                    ++q;
                }
                reqs[static_cast<std::size_t>(e)].push_back({v, q});
            }
        }
        for (std::size_t ci = 0; ci < capFracs.size(); ++ci) {
            const double capKB = capFracs[ci] * totalKB;
            const i64 units = 4096;
            const double unit = capKB / static_cast<double>(units);
            auto toUnits = [&](double kb) { return static_cast<i64>(std::ceil(kb / unit)); };
            std::map<std::string, std::array<double, 5>> acc;  // hits, reqs, bytesSaved, bytesReq, transcodeHits
            for (const auto& p : policies) acc[p] = {0, 0, 0, 0, 0};
            std::map<std::string, std::array<double, 2>> costAcc;  // saved $, base $
            std::map<std::string, std::array<double, 2>> benAcc;   // captured benefit, total benefit
            // online LRU over versions (warm across epochs)
            SizedLRU lru(static_cast<i64>(capKB));
            for (int e = 0; e < epochs; ++e) {
                const bool measure = e >= 1;
                // previous-epoch counts -> plans
                std::vector<std::vector<double>> f(static_cast<std::size_t>(V), std::vector<double>(static_cast<std::size_t>(Q), 0));
                if (e >= 1)
                    for (auto [v, q] : reqs[static_cast<std::size_t>(e - 1)]) f[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)] += 1;
                // cached[v] = highest cached version index (or -1); for independent KP a set
                std::map<std::string, std::vector<std::vector<char>>> cached;
                for (std::size_t pi = 1; pi < policies.size(); ++pi)
                    cached[policies[pi]].assign(static_cast<std::size_t>(V), std::vector<char>(static_cast<std::size_t>(Q), 0));
                if (e >= 1) {
                    // (a) independent items, value ignores transcoding
                    std::vector<knapsack::Item> items;
                    std::vector<std::pair<int, int>> ref;
                    for (int v = 0; v < V; ++v)
                        for (int q = 0; q < Q; ++q)
                            if (f[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)] > 0) {
                                items.push_back({toUnits(sizeKB[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)]),
                                                 f[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)] * benefit(v, q)});
                                ref.push_back({v, q});
                            }
                    for (int i : knapsack::solveAuto(items, units).chosen)
                        cached[policies[1]][static_cast<std::size_t>(ref[static_cast<std::size_t>(i)].first)][static_cast<std::size_t>(ref[static_cast<std::size_t>(i)].second)] = 1;
                    // (b) one item per video: the top requested version serves everything
                    items.clear();
                    ref.clear();
                    for (int v = 0; v < V; ++v) {
                        int top = -1;
                        double val = 0;
                        for (int q = 0; q < Q; ++q)
                            if (f[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)] > 0) top = q;
                        if (top < 0) continue;
                        for (int q = 0; q <= top; ++q)
                            val += f[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)] * benefit(v, q) * (q == top ? 1.0 : transcodeKeep);
                        items.push_back({toUnits(sizeKB[static_cast<std::size_t>(v)][static_cast<std::size_t>(top)]), val});
                        ref.push_back({v, top});
                    }
                    for (int i : knapsack::solveAuto(items, units).chosen)
                        cached[policies[2]][static_cast<std::size_t>(ref[static_cast<std::size_t>(i)].first)][static_cast<std::size_t>(ref[static_cast<std::size_t>(i)].second)] = 1;
                    // (c) MCKP: choose at most one version per video, transcode-aware value
                    std::vector<knapsack::Group> groups;
                    std::vector<int> gv;
                    for (int v = 0; v < V; ++v) {
                        double any = 0;
                        for (int q = 0; q < Q; ++q) any += f[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)];
                        if (any == 0) continue;
                        knapsack::Group g;
                        for (int j2 = 0; j2 < Q; ++j2) {
                            double val = 0;
                            for (int q = 0; q <= j2; ++q)
                                val += f[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)] * benefit(v, q) * (q == j2 ? 1.0 : transcodeKeep);
                            g.options.push_back({toUnits(sizeKB[static_cast<std::size_t>(v)][static_cast<std::size_t>(j2)]), val});
                        }
                        groups.push_back(std::move(g));
                        gv.push_back(v);
                    }
                    knapsack::MckpResult mr = knapsack::mckp(groups, units);
                    mckpMs = std::max(mckpMs, mr.millis);
                    mckpMem = std::max(mckpMem, mr.memoryBytes);
                    for (std::size_t g = 0; g < groups.size(); ++g)
                        if (mr.choice[g] >= 0) cached[policies[3]][static_cast<std::size_t>(gv[g])][static_cast<std::size_t>(mr.choice[g])] = 1;
                }
                // evaluate this epoch
                for (auto [v, q] : reqs[static_cast<std::size_t>(e)]) {
                    const double bytes = sizeKB[static_cast<std::size_t>(v)][static_cast<std::size_t>(q)];
                    const double dollars = bytes / (1024.0 * 1024.0) * price[static_cast<std::size_t>(v)];
                    for (std::size_t pi = 0; pi < policies.size(); ++pi) {
                        const auto& p = policies[pi];
                        int servedBy = -1;
                        if (pi == 0) {
                            for (int j2 = Q - 1; j2 >= q; --j2)
                                if (lru.contains(v * Q + j2)) {
                                    servedBy = j2;
                                    break;
                                }
                            if (servedBy >= 0) {
                                lru.touch(v * Q + servedBy);
                            } else {
                                lru.insert(v * Q + q, static_cast<i64>(bytes));
                            }
                        } else {
                            const auto& c = cached[p][static_cast<std::size_t>(v)];
                            if (c[static_cast<std::size_t>(q)]) servedBy = q;
                            else
                                for (int j2 = q + 1; j2 < Q; ++j2)
                                    if (c[static_cast<std::size_t>(j2)]) {
                                        servedBy = j2;
                                        break;
                                    }
                        }
                        if (!measure) continue;
                        auto& a = acc[p];
                        a[1] += 1;
                        a[3] += bytes;
                        costAcc[p][1] += dollars;
                        benAcc[p][1] += benefit(v, q);
                        if (servedBy >= 0) {
                            benAcc[p][0] += benefit(v, q) * (servedBy == q ? 1.0 : transcodeKeep);
                            a[0] += 1;
                            a[2] += bytes;
                            costAcc[p][0] += dollars;
                            if (servedBy != q) a[4] += 1;
                        }
                    }
                }
            }
            for (const auto& p : policies) {
                const auto& a = acc[p];
                hr[p][ci].push_back(a[0] / a[1]);
                bsaved[p][ci].push_back(a[2] / a[3]);
                trans[p][ci].push_back(a[0] > 0 ? 100.0 * a[4] / a[0] : 0);
                cost[p][ci].push_back(100.0 * costAcc[p][0] / costAcc[p][1]);
                bene[p][ci].push_back(benAcc[p][0] / benAcc[p][1]);
            }
        }
    }
    JsonWriter j;
    j.beginObject().kv("experiment", "abr").kv("seeds", seeds).kv("videos", V);
    j.kv("transcodeKeep", transcodeKeep).kv("mckpMaxMs", mckpMs).kv("mckpMemKB", static_cast<double>(mckpMem) / 1024.0);
    j.key("ladder").beginArray();
    for (std::size_t q = 0; q < ladder.size(); ++q)
        j.beginObject().kv("name", ladder[q].name).kv("kbps", ladder[q].kbps).kv("share", deviceMix[q]).endObject();
    j.endArray();
    j.array("x", capFracs);
    j.kv("xLabel", "cache size (fraction of all renditions)");
    j.key("policies").beginArray();
    for (const auto& p : policies) {
        j.beginObject().kv("name", p);
        auto emit = [&](const char* k, std::vector<std::vector<double>>& m) {
            std::vector<double> mean, sd;
            for (auto& xs : m) {
                auto ms = Agg::meanStd(xs);
                mean.push_back(ms.first);
                sd.push_back(ms.second);
            }
            j.array(std::string(k), mean).array(std::string(k) + "Std", sd);
        };
        emit("benefitRatio", bene[p]);
        emit("hitRatio", hr[p]);
        emit("byteHitRatio", bsaved[p]);
        emit("transcodePct", trans[p]);
        emit("costSavedPct", cost[p]);
        j.endObject();
    }
    j.endArray().endObject();
    return {writeFile(opt, "abr.json", j.str())};
}

// ---------------------------------------------------------------- optgap --
// "Price of approximation" vs "price of uncertainty": how far is greedy from the
// exact knapsack optimum on real cache instances, and how does that compare to
// the theoretical bound OPT - greedy <= v_break?
std::vector<std::string> optGapExperiment(const ExperimentOptions& opt) {
    std::cerr << "[optgap] greedy / FPTAS / unit-DP gap vs exact optimum on real cache instances\n";
    const std::vector<double> caps = {0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2};
    const int seeds = seedsFor(opt, 4);
    const std::vector<std::string> methods = {"greedy", "modgreedy", "fptas:0.05", "unitDP-4096", "unitDP-4096+fill"};
    std::map<std::string, std::vector<std::vector<double>>> gap;
    for (const auto& m : methods) gap[m].assign(caps.size(), {});
    std::vector<std::vector<double>> bound(caps.size()), bnbMs(caps.size()), nItems(caps.size()), proven(caps.size()),
        lpSlack(caps.size()), bnbGap(caps.size());
    std::mutex mu;
    struct Task {
        int seed;
        int epoch;
    };
    std::vector<Task> tasks;
    for (int s = 1; s <= seeds; ++s)
        for (int e : {4, 8, 12}) tasks.push_back({s, e});
    parallelFor(
        static_cast<int>(tasks.size()),
        [&](int ti) {
            const Task t = tasks[static_cast<std::size_t>(ti)];
            CatalogConfig cc;
            cc.numObjects = opt.quick ? 1500 : 5000;
            cc.seed = static_cast<std::uint64_t>(t.seed) * 7919 + 13;
            Catalog cat = Catalog::synthetic(cc);
            const int epochLen = opt.quick ? 5000 : 15000;
            Trace tr = buildTrace(cat, presetTraffic("zipf"), t.epoch + 1, epochLen, static_cast<std::uint64_t>(t.seed));
            std::vector<double> f(cat.size(), 0);
            for (std::size_t i = tr.epochStart[static_cast<std::size_t>(t.epoch)]; i < tr.epochStart[static_cast<std::size_t>(t.epoch) + 1]; ++i)
                f[static_cast<std::size_t>(tr.requests[i].objectId)] += 1;
            std::vector<knapsack::Item> items;
            for (std::size_t k = 0; k < f.size(); ++k)
                if (f[k] > 0) items.push_back({cat.objects[k].sizeKB, f[k] * cat.hitBenefit(cat.objects[k], BenefitWeights{})});
            for (std::size_t ci = 0; ci < caps.size(); ++ci) {
                const i64 C = static_cast<i64>(caps[ci] * static_cast<double>(cat.totalKB));
                knapsack::Result ex = knapsack::branchAndBound(items, C, 20'000'000);
                const double U = knapsack::dantzigBound(items, C);  // certified: OPT <= U
                // break item of the density order
                std::vector<int> ord(items.size());
                std::iota(ord.begin(), ord.end(), 0);
                std::sort(ord.begin(), ord.end(), [&](int a, int b) {
                    return items[static_cast<std::size_t>(a)].value * static_cast<double>(items[static_cast<std::size_t>(b)].weight) >
                           items[static_cast<std::size_t>(b)].value * static_cast<double>(items[static_cast<std::size_t>(a)].weight);
                });
                double vb = 0;
                i64 cum = 0;
                for (int i : ord) {
                    if (items[static_cast<std::size_t>(i)].weight > C) continue;
                    cum += items[static_cast<std::size_t>(i)].weight;
                    if (cum > C) {
                        vb = items[static_cast<std::size_t>(i)].value;
                        break;
                    }
                }
                std::map<std::string, double> g;
                g["greedy"] = knapsack::greedyDensity(items, C).value;
                g["modgreedy"] = knapsack::modifiedGreedy(items, C).value;
                g["fptas:0.05"] = knapsack::fptas(items, C, 0.05).value;
                {
                    const i64 unit = std::max<i64>(1, (C + 4095) / 4096);
                    std::vector<knapsack::Item> u(items.size());
                    for (std::size_t i = 0; i < items.size(); ++i) u[i] = {(items[i].weight + unit - 1) / unit, items[i].value};
                    knapsack::Result r = knapsack::dpRolling(u, C / unit);
                    g["unitDP-4096"] = r.value;
                    std::vector<char> in(items.size(), 0);
                    i64 used = 0;
                    double v = 0;
                    for (int i : r.chosen) {
                        in[static_cast<std::size_t>(i)] = 1;
                        used += items[static_cast<std::size_t>(i)].weight;
                        v += items[static_cast<std::size_t>(i)].value;
                    }
                    for (int i : ord)
                        if (!in[static_cast<std::size_t>(i)] && used + items[static_cast<std::size_t>(i)].weight <= C) {
                            used += items[static_cast<std::size_t>(i)].weight;
                            v += items[static_cast<std::size_t>(i)].value;
                        }
                    g["unitDP-4096+fill"] = v;
                }
                double OPT = ex.value;  // best known lower bound (exact when B&B proved it)
                for (const auto& kv : g) OPT = std::max(OPT, kv.second);
                std::lock_guard<std::mutex> lk(mu);
                for (const auto& m : methods) gap[m][ci].push_back(OPT > 0 ? 100.0 * (1.0 - g[m] / OPT) : 0.0);
                bnbGap[ci].push_back(OPT > 0 ? 100.0 * (1.0 - ex.value / OPT) : 0.0);
                lpSlack[ci].push_back(U > 0 ? 100.0 * (1.0 - OPT / U) : 0.0);
                bound[ci].push_back(OPT > 0 ? 100.0 * vb / OPT : 0.0);
                bnbMs[ci].push_back(ex.millis);
                nItems[ci].push_back(static_cast<double>(items.size()));
                proven[ci].push_back(ex.provenOptimal ? 1.0 : 0.0);
            }
        },
        opt.threads);
    JsonWriter j;
    j.beginObject().kv("experiment", "optgap").kv("instances", static_cast<long long>(tasks.size()));
    j.array("x", caps).kv("xLabel", "cache size (fraction of catalog bytes)");
    auto meanOf = [](std::vector<std::vector<double>>& m, bool maxInstead) {
        std::vector<double> out;
        for (auto& xs : m) {
            if (maxInstead) out.push_back(*std::max_element(xs.begin(), xs.end()));
            else out.push_back(Agg::meanStd(xs).first);
        }
        return out;
    };
    j.key("methods").beginArray();
    for (const auto& m : methods) {
        j.beginObject().kv("name", m);
        j.array("meanGapPct", meanOf(gap[m], false)).array("maxGapPct", meanOf(gap[m], true));
        j.endObject();
    }
    j.endArray();
    j.array("boundPct", meanOf(bound, false));
    j.array("lpSlackPct", meanOf(lpSlack, false));   // true gap <= reported gap + lpSlack
    j.array("lpSlackMaxPct", meanOf(lpSlack, true));
    j.array("bnbGapPct", meanOf(bnbGap, false));
    j.array("bnbMs", meanOf(bnbMs, false));
    j.array("items", meanOf(nItems, false));
    j.array("provenFraction", meanOf(proven, false));
    j.endObject();
    return {writeFile(opt, "optgap.json", j.str())};
}

}  // namespace

std::vector<std::string> experimentNames() {
    return {"compare", "capacity", "skew", "flash", "epoch", "weights", "ablation", "resolution", "optgap", "solvers", "abr"};
}

std::vector<std::string> runExperiment(const std::string& name, const ExperimentOptions& opt) {
    std::vector<std::string> files;
    auto append = [&](std::vector<std::string> f) { files.insert(files.end(), f.begin(), f.end()); };
    const auto defaults = defaultStrategyKeys();

    if (name == "all") {
        for (const auto& n : experimentNames()) append(runExperiment(n, opt));
        // index of everything written
        JsonWriter j;
        j.beginObject().kv("quick", opt.quick);
        j.key("files").beginArray();
        for (const auto& f : files) j.value(fs::path(f).filename().string());
        j.endArray().endObject();
        files.push_back(writeFile(opt, "index.json", j.str()));
        return files;
    }
    if (name == "compare") {
        append(compareLike("compare", {"uniform", "zipf", "flash", "drift", "mixed"}, defaults, seedsFor(opt, 5), true, opt));
    } else if (name == "ablation") {
        append(compareLike("ablation", {"zipf", "flash", "drift", "mixed"},
                           {"dp-reactive", "dp-predictive", "pack-noforecast", "pack-nochurn", "pack-nodrift",
                            "pack-window", "pack-greedy", "pack-fptas", "pack"},
                           seedsFor(opt, 5), false, opt));
    } else if (name == "capacity") {
        append(sweep("capacity", "cache size (fraction of catalog bytes)", {0.005, 0.01, 0.02, 0.05, 0.1, 0.2},
                     {"zipf", "mixed"}, defaults, seedsFor(opt, 3), [](SimConfig& c, double x) { c.capacityFrac = x; }, opt));
    } else if (name == "skew") {
        append(sweep("skew", "Zipf exponent s", {0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4}, {"zipf"},
                     {"lru", "lfu", "gdsf", "greedy-epoch", "dp-reactive", "pack", "dp-oracle", "belady"}, seedsFor(opt, 3),
                     [](SimConfig& c, double x) { c.traffic.zipfS = x; }, opt));
    } else if (name == "flash") {
        append(sweep("flash", "flash-crowd peak share of traffic", {0.1, 0.2, 0.3, 0.45, 0.6, 0.75}, {"flash"},
                     {"lru", "lfu", "gdsf", "dp-reactive", "dp-predictive", "pack", "dp-oracle", "belady"}, seedsFor(opt, 3),
                     [](SimConfig& c, double x) { c.traffic.flashIntensity = x; }, opt));
    } else if (name == "epoch") {
        const bool q = opt.quick;
        append(sweep("epoch", "epoch length (requests between re-plans)",
                     q ? std::vector<double>{2000, 5000, 10000} : std::vector<double>{2500, 5000, 10000, 20000, 40000, 80000},
                     {"drift", "flash"}, {"lru", "greedy-epoch", "dp-reactive", "dp-predictive", "pack", "dp-oracle"},
                     seedsFor(opt, 3),
                     [q](SimConfig& c, double x) {
                         const int total = q ? 80000 : 640000;
                         c.epochLen = static_cast<int>(x);
                         c.epochs = std::max(3, total / c.epochLen);
                         c.warmup = std::max(1, static_cast<int>(std::ceil((q ? 10000.0 : 40000.0) / x)));
                     },
                     opt));
    } else if (name == "weights") {
        append(sweep("weights", "alpha (bandwidth weight); beta = 1 - alpha",
                     {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0}, {"zipf", "mixed"},
                     {"lru", "gdsf", "dp-reactive", "pack", "dp-oracle"}, seedsFor(opt, 2),
                     [](SimConfig& c, double x) {
                         c.weights.alpha = x;
                         c.weights.beta = 1.0 - x;
                     },
                     opt));
    } else if (name == "resolution") {
        append(sweep("resolution", "DP capacity resolution (max units)", {128, 256, 512, 1024, 2048, 4096, 8192, 16384},
                     {"zipf"}, {"greedy-epoch", "dp-units", "dp-reactive", "dp-oracle"}, seedsFor(opt, 2),
                     [](SimConfig& c, double x) { c.maxUnits = static_cast<i64>(x); }, opt));
    } else if (name == "optgap") {
        append(optGapExperiment(opt));
    } else if (name == "solvers") {
        append(solversExperiment(opt));
    } else if (name == "abr") {
        append(abrExperiment(opt));
    } else {
        throw std::invalid_argument("unknown experiment: " + name);
    }
    return files;
}

}  // namespace cdn
