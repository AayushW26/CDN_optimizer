// cdnopt -- command-line entry point of the CDN Cache Optimizer.
//
//   cdnopt run        [options]            simulate one scenario, print / write JSON
//   cdnopt experiment <name|all> [--quick] run the report's experiment matrix
//   cdnopt solve      [--n --cap --algo]   compare knapsack solvers on one instance
//   cdnopt list                            list strategies, patterns, experiments
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "cdn/Experiments.hpp"
#include "cdn/Knapsack.hpp"
#include "cdn/Simulation.hpp"

using namespace cdn;

namespace {

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;

    bool has(const std::string& k) const { return opts.count(k) > 0; }
    std::string str(const std::string& k, const std::string& d) const {
        auto it = opts.find(k);
        return it == opts.end() ? d : it->second;
    }
    double num(const std::string& k, double d) const {
        auto it = opts.find(k);
        return it == opts.end() ? d : std::stod(it->second);
    }
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string key = s.substr(2), val = "1";
            auto eq = key.find('=');
            if (eq != std::string::npos) {
                val = key.substr(eq + 1);
                key = key.substr(0, eq);
            } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                val = argv[++i];
            }
            a.opts[key] = val;
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ',')) if (!t.empty()) out.push_back(t);
    return out;
}

void usage() {
    std::cout << R"(CDN Cache Optimizer -- knapsack DP vs. online caching

usage:
  cdnopt run [options]
      --pattern   uniform|zipf|flash|drift|mixed     (default zipf)
      --trace     access-log.csv (object_id,size_kb)  replay a real trace instead
      --objects   catalog size                        (default 5000)
      --epochs    number of epochs                    (default 40)
      --epoch-len requests per epoch                  (default 15000)
      --capacity  cache size, fraction of catalog     (default 0.05)
      --alpha     bandwidth weight                    (default 1)
      --beta      latency weight                      (default 1)
      --zipf      Zipf exponent                       (default preset)
      --flash     flash-crowd peak traffic share      (default preset)
      --units     DP capacity resolution              (default 4096)
      --algos     comma list or 'all'                 (default: main set)
      --seed      RNG seed                            (default 1)
      --out       file.json or '-' for stdout         (default: table only)
      --detail    include per-epoch series / cache contents / plan log

  cdnopt experiment <name|all> [--out-dir results] [--quick] [--threads N]
      names: compare capacity skew flash epoch weights ablation resolution optgap solvers abr

  cdnopt solve [--n 2000] [--cap 8192] [--seed 1] [--correlated] [--algos a,b,...]

  cdnopt list
)";
}

int cmdRun(const Args& a) {
    SimConfig cfg;
    const std::string pattern = a.str("pattern", "zipf");
    if (a.has("trace")) {
        cfg.traceCsv = a.str("trace", "");
        cfg.traffic = presetTraffic("zipf");
        cfg.traffic.pattern = "trace";
    } else {
        cfg.traffic = presetTraffic(pattern);
    }
    cfg.numObjects = static_cast<int>(a.num("objects", cfg.numObjects));
    cfg.epochs = static_cast<int>(a.num("epochs", cfg.epochs));
    cfg.epochLen = static_cast<int>(a.num("epoch-len", cfg.epochLen));
    cfg.capacityFrac = a.num("capacity", cfg.capacityFrac);
    cfg.weights.alpha = a.num("alpha", 1.0);
    cfg.weights.beta = a.num("beta", 1.0);
    cfg.seed = static_cast<std::uint64_t>(a.num("seed", 1));
    cfg.maxUnits = static_cast<i64>(a.num("units", 4096));
    cfg.warmup = static_cast<int>(a.num("warmup", cfg.warmup));
    cfg.threads = static_cast<int>(a.num("threads", 0));
    if (a.has("zipf")) cfg.traffic.zipfS = a.num("zipf", cfg.traffic.zipfS);
    if (a.has("flash")) cfg.traffic.flashIntensity = a.num("flash", cfg.traffic.flashIntensity);
    if (a.has("catalog")) cfg.catalogCsv = a.str("catalog", "");
    if (a.has("algos")) {
        const std::string s = a.str("algos", "");
        cfg.algos = s == "all" ? allStrategyKeys() : split(s);
    }
    if (cfg.epochs <= 0 || cfg.epochLen <= 0 || cfg.numObjects <= 0 || cfg.capacityFrac <= 0) {
        std::cerr << "invalid numeric option\n";
        return 2;
    }
    if (cfg.epochs * static_cast<double>(cfg.epochLen) > 2e7) {
        std::cerr << "refusing to simulate more than 2e7 requests\n";
        return 2;
    }

    SimRun run = SimulationEngine::run(cfg);
    const std::string out = a.str("out", "");
    if (out == "-") {
        JsonWriter j;
        writeRun(j, run, a.has("detail"));
        std::cout << j.str() << "\n";
        return 0;
    }
    std::cout << "pattern=" << cfg.traffic.pattern << "  objects=" << run.catalog->size()
              << "  cache=" << std::fixed << std::setprecision(2) << run.capacityKB / 1048576.0 << " GB ("
              << cfg.capacityFrac * 100 << "% of " << run.catalog->totalKB / 1048576.0 << " GB)"
              << "  requests=" << run.totalRequests << " (" << run.cfg.epochs << " epochs)  wall=" << run.wallMs / 1000
              << " s\n\n";
    std::cout << std::left << std::setw(22) << "algorithm" << std::right << std::setw(9) << "benefit%" << std::setw(9) << "hit%" << std::setw(9)
              << "byte%" << std::setw(11) << "lat(ms)" << std::setw(11) << "$saved%" << std::setw(11) << "origin GB"
              << std::setw(10) << "plan ms" << std::setw(9) << "ns/req" << "\n";
    for (const auto& r : run.algos) {
        const Summary& s = r.summary;
        std::cout << std::left << std::setw(22) << r.name << std::right << std::setprecision(2) << std::setw(9)
                  << 100 * s.benefitRatio << std::setw(9) << 100 * s.hitRatio << std::setw(9) << 100 * s.byteHitRatio << std::setw(11) << s.avgLatencyMs
                  << std::setw(11) << s.costSavedPct << std::setw(11) << s.originGB << std::setprecision(1)
                  << std::setw(10) << s.planMs << std::setprecision(0) << std::setw(9) << s.nsPerRequest << "\n";
    }
    std::cout << "\n(* = oracle with future knowledge; reference bound, not deployable)\n";
    if (!out.empty()) {
        JsonWriter j;
        writeRun(j, run, a.has("detail"));
        std::ofstream f(out);
        f << j.str() << "\n";
        std::cout << "wrote " << out << "\n";
    }
    return 0;
}

int cmdSolve(const Args& a) {
    const int n = static_cast<int>(a.num("n", 2000));
    const i64 C = static_cast<i64>(a.num("cap", 8192));
    Rng rng(static_cast<std::uint64_t>(a.num("seed", 1)));
    std::vector<knapsack::Item> items(static_cast<std::size_t>(n));
    for (auto& it : items) {
        it.weight = rng.intIn(1, std::max(2, static_cast<int>(C / 20)));
        it.value = a.has("correlated") ? static_cast<double>(it.weight) + C / 200.0 : rng.uniform(1, 1000);
    }
    std::vector<std::string> algos = split(a.str("algos", "rolling,hirschberg,bnb,fptas:0.1,fptas:0.01,greedy,modgreedy"));
    const double ub = knapsack::dantzigBound(items, C);
    std::cout << "n=" << n << " C=" << C << " LP bound=" << std::fixed << std::setprecision(2) << ub << "\n\n";
    std::cout << std::left << std::setw(14) << "algorithm" << std::right << std::setw(16) << "value" << std::setw(12)
              << "gap to LP%" << std::setw(12) << "ms" << std::setw(14) << "memory KB" << std::setw(10) << "exact" << "\n";
    for (const auto& al : algos) {
        try {
            auto r = knapsack::solve(al, items, C);
            std::cout << std::left << std::setw(14) << al << std::right << std::setw(16) << r.value << std::setw(12)
                      << std::setprecision(4) << 100 * (1 - r.value / ub) << std::setprecision(2) << std::setw(12)
                      << r.millis << std::setw(14) << r.memoryBytes / 1024.0 << std::setw(10)
                      << (r.provenOptimal ? "yes" : "-") << "\n";
        } catch (const std::exception& e) {
            std::cout << std::left << std::setw(14) << al << "  skipped: " << e.what() << "\n";
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    if (a.positional.empty() || a.has("help")) {
        usage();
        return a.positional.empty() ? 1 : 0;
    }
    try {
        const std::string cmd = a.positional[0];
        if (cmd == "run") return cmdRun(a);
        if (cmd == "solve") return cmdSolve(a);
        if (cmd == "experiment") {
            ExperimentOptions opt;
            opt.outDir = a.str("out-dir", "results");
            opt.quick = a.has("quick");
            opt.threads = static_cast<int>(a.num("threads", 0));
            const std::string name = a.positional.size() > 1 ? a.positional[1] : "all";
            runExperiment(name, opt);
            return 0;
        }
        if (cmd == "list") {
            std::cout << "strategies: ";
            for (const auto& k : allStrategyKeys()) std::cout << k << " ";
            std::cout << "\npatterns:   uniform zipf flash drift mixed (or --trace file.csv)\nexperiments: ";
            for (const auto& k : experimentNames()) std::cout << k << " ";
            std::cout << "all\n";
            return 0;
        }
        usage();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
