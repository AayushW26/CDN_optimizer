// Policy registry: name -> policy instance.
#pragma once

#include <map>
#include <sstream>

#include "policies_advanced.hpp"
#include "policies_basic.hpp"
#include "shade.hpp"

namespace lab {

inline std::map<std::string, ShadeParams> shadeVariants() {
    std::map<std::string, ShadeParams> v;
    ShadeParams base;
    v["SHADE"] = base;
    ShadeParams b = base; b.byteObjective = true; b.label = "SHADE-B"; v["SHADE-B"] = b;
    ShadeParams na = base; na.admission = false; na.label = "SHADE-noAdmit"; v["SHADE-noAdmit"] = na;
    ShadeParams ng = base; ng.useGap = false; ng.label = "SHADE-noGap"; v["SHADE-noGap"] = ng;
    ShadeParams nc = base; nc.useCount = false; nc.label = "SHADE-noCount"; v["SHADE-noCount"] = nc;
    ShadeParams ns = base; ns.useSize = false; ns.label = "SHADE-noSize"; v["SHADE-noSize"] = ns;
    ShadeParams nf = base; nf.useCount = false; nf.useGap = false; nf.useSize = false; nf.label = "SHADE-noFeatures"; v["SHADE-noFeatures"] = nf;
    return v;
}

inline std::vector<std::string> allPolicyNames() {
    return {"LRU", "FIFO", "SIEVE", "S3-FIFO", "GDSF", "W-TinyLFU", "AdaptSize", "LHD", "SHADE", "Belady-Size*"};
}

inline std::unique_ptr<Policy> makePolicy(const std::string& name, const PolicyContext& ctx) {
    if (name == "LRU") return std::make_unique<LruPolicy>(ctx, false);
    if (name == "FIFO") return std::make_unique<LruPolicy>(ctx, true);
    if (name == "SIEVE") return std::make_unique<SievePolicy>(ctx);
    if (name == "S3-FIFO") return std::make_unique<S3FifoPolicy>(ctx);
    if (name == "GDSF") return std::make_unique<GdsfPolicy>(ctx);
    if (name == "LHD") return std::make_unique<LhdPolicy>(ctx);
    if (name == "AdaptSize") return std::make_unique<AdaptSizePolicy>(ctx);
    if (name == "W-TinyLFU") {
        const double avg = static_cast<double>(ctx.trace->footprintBytes) / std::max<uint32_t>(1, ctx.trace->numObjects);
        return std::make_unique<WTinyLfuPolicy>(ctx, static_cast<uint64_t>(ctx.capacity / std::max(1.0, avg)) + 1);
    }
    if (name == "Belady-Size*") return std::make_unique<BeladySizePolicy>(ctx);
    auto v = shadeVariants();
    auto it = v.find(name);
    if (it != v.end()) return std::make_unique<ShadePolicy>(ctx, it->second);
    // SHADE with overrides: SHADE:key=value:key=value
    if (name.rfind("SHADE:", 0) == 0) {
        ShadeParams p;
        p.label = name;
        std::stringstream ss(name.substr(6));
        std::string kv;
        while (std::getline(ss, kv, ':')) {
            const auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq);
            const double val = std::atof(kv.substr(eq + 1).c_str());
            if (k == "h") { p.horizonFactor = val; p.indexRank = false; }
            else if (k == "index") p.indexRank = val != 0;
            else if (k == "km") p.kaplanMeier = val != 0;
            else if (k == "pool") p.poolKeep = static_cast<uint32_t>(val);
            else if (k == "shrink") p.shrink = val;
            else if (k == "exchange") p.exchange = val != 0;
            else if (k == "size") p.useSize = val != 0;
            else if (k == "hl") p.halfLifeLog2 = val;
            else if (k == "slack") p.admitSlack = val;
            else if (k == "samples") p.samples = static_cast<uint32_t>(val);
            else if (k == "hist") p.historyLog2 = static_cast<uint32_t>(val);
            else if (k == "admit") p.admission = val != 0;
            else if (k == "byte") p.byteObjective = val != 0;
            else throw std::runtime_error("unknown SHADE parameter: " + k);
        }
        return std::make_unique<ShadePolicy>(ctx, p);
    }
    throw std::runtime_error("unknown policy: " + name);
}

}  // namespace lab
