// cachelab: trace-driven evaluation of CDN caching policies on real request traces.
//
//   cachelab convert <in.txt> <out.bin>                 text "time object size" -> binary
//   cachelab stats   <trace.bin> [--offset N] [--limit N]
//   cachelab run     <trace.bin> [--offset N] [--limit N] [--sizes 0.001,0.01,0.1]
//                    [--policies a,b,...|all] [--threads 4] [--out results.json] [--seed 42]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "bounds.hpp"
#include "registry.hpp"


using namespace lab;

static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string x;
    while (std::getline(ss, x, d))
        if (!x.empty()) out.push_back(x);
    return out;
}

struct Args {
    std::map<std::string, std::string> kv;
    std::vector<std::string> pos;
    Args(int argc, char** argv, int from) {
        for (int i = from; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) == 0 && i + 1 < argc) kv[a.substr(2)] = argv[++i];
            else pos.push_back(a);
        }
    }
    std::string get(const std::string& k, const std::string& d) const {
        auto it = kv.find(k);
        return it == kv.end() ? d : it->second;
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: cachelab convert|stats|run ...\n";
        return 1;
    }
    const std::string cmd = argv[1];
    Args a(argc, argv, 2);
    try {
        if (cmd == "convert") {
            if (a.pos.size() < 2) throw std::runtime_error("convert <in.txt> <out.bin>");
            std::ifstream in(a.pos[0]);
            if (!in) throw std::runtime_error("cannot open " + a.pos[0]);
            ConvertStats st;
            Trace t = convertText(in, st);
            saveBinary(t, a.pos[1]);
            std::printf("lines=%llu kept=%llu zero-size=%llu malformed=%llu objects=%u\n",
                        (unsigned long long)st.lines, (unsigned long long)st.kept,
                        (unsigned long long)st.zeroSize, (unsigned long long)st.malformed, t.numObjects);
            return 0;
        }
        if (a.pos.empty()) throw std::runtime_error("missing trace path");
        const std::size_t offset = std::stoull(a.get("offset", "0"));
        const std::size_t limit = std::stoull(a.get("limit", "0"));
        Trace t = loadBinary(a.pos[0], offset, limit);
        uint64_t oneHit = 0;
        {
            std::vector<uint32_t> cnt(t.numObjects, 0);
            for (uint32_t x : t.id) ++cnt[x];
            for (uint32_t c : cnt) oneHit += (c == 1);
        }
        std::printf("trace=%s offset=%zu requests=%zu objects=%u footprint=%.2fGB requested=%.2fGB one-hit-objects=%.1f%%\n",
                    a.pos[0].c_str(), offset, t.n(), t.numObjects, t.footprintBytes / 1e9, t.totalBytes / 1e9,
                    100.0 * oneHit / std::max<uint32_t>(1, t.numObjects));
        if (cmd == "stats") return 0;
        if (cmd != "run") throw std::runtime_error("unknown command " + cmd);

        std::vector<double> sizes;
        for (auto& s : split(a.get("sizes", "0.001,0.01,0.1"), ',')) sizes.push_back(std::atof(s.c_str()));
        std::vector<std::string> pols = a.get("policies", "all") == "all" ? allPolicyNames() : split(a.get("policies", ""), ',');
        const int threads = std::atoi(a.get("threads", "4").c_str());
        const uint64_t seed = std::stoull(a.get("seed", "42"));
        // Requests before `warmup` (fraction of the trace) are simulated but not counted.
        const std::size_t warm = static_cast<std::size_t>(std::atof(a.get("warmup", "0").c_str()) * t.n());
        uint64_t measuredReqs = 0, measuredBytes = 0;
        for (std::size_t i = warm; i < t.n(); ++i) { ++measuredReqs; measuredBytes += t.size[i]; }

        struct Job { std::string policy; double frac; };
        struct Res { std::string policy; double frac; uint64_t cap; double omr, bmr, secs; };
        std::vector<Job> jobs;
        for (double f : sizes)
            for (auto& p : pols) jobs.push_back({p, f});
        std::vector<Res> results(jobs.size());
        std::atomic<std::size_t> nextJob{0};
        std::mutex io;
        auto worker = [&]() {
            for (;;) {
                const std::size_t j = nextJob++;
                if (j >= jobs.size()) return;
                const auto t0 = std::chrono::steady_clock::now();
                PolicyContext ctx;
                ctx.trace = &t;
                ctx.capacity = static_cast<uint64_t>(jobs[j].frac * static_cast<double>(t.footprintBytes));
                ctx.seed = seed;
                auto pol = makePolicy(jobs[j].policy, ctx);
                uint64_t hits = 0, hitBytes = 0;
                for (std::size_t i = 0; i < t.n(); ++i) {
                    if (pol->access(i, t.id[i], t.size[i]) && i >= warm) {
                        ++hits;
                        hitBytes += t.size[i];
                    }
                    if (pol->usedBytes() > ctx.capacity) throw std::runtime_error(pol->name() + " exceeded capacity");
                }
                const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                Res r{pol->name(), jobs[j].frac, ctx.capacity, 1.0 - double(hits) / double(measuredReqs),
                      1.0 - double(hitBytes) / double(measuredBytes), secs};
                results[j] = r;
                std::lock_guard<std::mutex> g(io);
                std::printf("  %-18s cache=%6.3f%%  OMR=%.4f  BMR=%.4f  (%.1fs)\n", r.policy.c_str(), 100 * r.frac,
                            r.omr, r.bmr, r.secs);
                std::fflush(stdout);
            }
        };
        std::vector<std::thread> ws;
        for (int k = 0; k < std::max(1, threads); ++k) ws.emplace_back(worker);
        for (auto& w : ws) w.join();

        std::vector<BoundResult> bounds;
        for (double f : sizes) bounds.push_back(pfooL(t, static_cast<uint64_t>(f * double(t.footprintBytes))));

        std::printf("\n%-18s", "miss ratio (obj)");
        for (double f : sizes) std::printf("  %8.3f%%", 100 * f);
        std::printf("\n");
        for (auto& p : pols) {
            std::string nm;
            for (std::size_t j = 0; j < jobs.size(); ++j)
                if (jobs[j].policy == p) nm = results[j].policy;
            std::printf("%-18s", nm.c_str());
            for (double f : sizes)
                for (std::size_t j = 0; j < jobs.size(); ++j)
                    if (jobs[j].policy == p && jobs[j].frac == f) std::printf("  %9.4f", results[j].omr);
            std::printf("\n");
        }
        std::printf("%-18s", "PFOO-L bound");
        for (auto& b : bounds) std::printf("  %9.4f", 1 - b.hitRatio);
        std::printf("\n");

        const std::string out = a.get("out", "");
        if (!out.empty()) {
            std::ofstream o(out);
            o << "{\"trace\":\"" << a.pos[0] << "\",\"offset\":" << offset << ",\"requests\":" << t.n()
              << ",\"objects\":" << t.numObjects << ",\"footprintBytes\":" << t.footprintBytes
              << ",\"requestedBytes\":" << t.totalBytes << ",\"oneHitObjectFrac\":"
              << double(oneHit) / std::max<uint32_t>(1, t.numObjects) << ",\"results\":[";
            for (std::size_t j = 0; j < results.size(); ++j) {
                const auto& r = results[j];
                o << (j ? "," : "") << "{\"policy\":\"" << r.policy << "\",\"cacheFrac\":" << r.frac
                  << ",\"capacityBytes\":" << r.cap << ",\"objectMissRatio\":" << r.omr
                  << ",\"byteMissRatio\":" << r.bmr << ",\"seconds\":" << r.secs << "}";
            }
            o << "],\"bounds\":[";
            for (std::size_t k = 0; k < sizes.size(); ++k)
                o << (k ? "," : "") << "{\"cacheFrac\":" << sizes[k] << ",\"objectMissRatio\":"
                  << 1 - bounds[k].hitRatio << ",\"byteMissRatio\":" << 1 - bounds[k].byteHitRatio << "}";
            o << "]}\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
