// Trace format and loading for the cache lab.
//
// A trace is a sequence of requests (object, size). Object ids are remapped to dense
// integers 0..numObjects-1 when a text trace is converted, so policies can keep their
// per-object state in flat arrays instead of hash maps. Time is logical: request i
// happens at time i.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace lab {

constexpr uint32_t kNever = std::numeric_limits<uint32_t>::max();

struct Trace {
    std::string name;
    std::vector<uint32_t> id;    // dense object id per request
    std::vector<uint32_t> size;  // bytes per request
    uint32_t numObjects = 0;

    // Derived (computeDerived):
    std::vector<uint32_t> next;  // index of the next request to the same object, or kNever
    uint64_t footprintBytes = 0; // sum of sizes of distinct objects
    uint64_t totalBytes = 0;     // sum of sizes of all requests

    std::size_t n() const { return id.size(); }

    void computeDerived() {
        const std::size_t N = n();
        next.assign(N, kNever);
        std::vector<uint32_t> last(numObjects, kNever);
        std::vector<char> seen(numObjects, 0);
        footprintBytes = totalBytes = 0;
        for (std::size_t i = N; i-- > 0;) {
            next[i] = last[id[i]];
            last[id[i]] = static_cast<uint32_t>(i);
        }
        for (std::size_t i = 0; i < N; ++i) {
            totalBytes += size[i];
            if (!seen[id[i]]) {
                seen[id[i]] = 1;
                footprintBytes += size[i];
            }
        }
    }
};

// Binary layout: "CLAB1\0\0\0", uint64 N, uint32 numObjects, uint32 pad, id[N], size[N].
inline void saveBinary(const Trace& t, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);
    const char magic[8] = {'C', 'L', 'A', 'B', '1', 0, 0, 0};
    out.write(magic, 8);
    const uint64_t N = t.n();
    const uint32_t pad = 0;
    out.write(reinterpret_cast<const char*>(&N), 8);
    out.write(reinterpret_cast<const char*>(&t.numObjects), 4);
    out.write(reinterpret_cast<const char*>(&pad), 4);
    out.write(reinterpret_cast<const char*>(t.id.data()), static_cast<std::streamsize>(N * 4));
    out.write(reinterpret_cast<const char*>(t.size.data()), static_cast<std::streamsize>(N * 4));
}

inline Trace loadBinary(const std::string& path, std::size_t offset = 0, std::size_t limit = 0) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    char magic[8];
    in.read(magic, 8);
    if (std::string(magic, 5) != "CLAB1") throw std::runtime_error("not a cache-lab trace: " + path);
    uint64_t N = 0;
    uint32_t numObjects = 0, pad = 0;
    in.read(reinterpret_cast<char*>(&N), 8);
    in.read(reinterpret_cast<char*>(&numObjects), 4);
    in.read(reinterpret_cast<char*>(&pad), 4);
    if (offset > N) offset = N;
    std::size_t count = N - offset;
    if (limit && limit < count) count = limit;
    std::vector<uint32_t> ids(count), sizes(count);
    in.seekg(static_cast<std::streamoff>(24 + offset * 4));
    in.read(reinterpret_cast<char*>(ids.data()), static_cast<std::streamsize>(count * 4));
    in.seekg(static_cast<std::streamoff>(24 + N * 4 + offset * 4));
    in.read(reinterpret_cast<char*>(sizes.data()), static_cast<std::streamsize>(count * 4));
    if (!in) throw std::runtime_error("truncated trace: " + path);

    // Re-densify ids so a slice only allocates state for the objects it touches.
    Trace t;
    t.name = path;
    std::vector<uint32_t> remap(numObjects, kNever);
    t.id.resize(count);
    t.size = std::move(sizes);
    uint32_t nextId = 0;
    for (std::size_t i = 0; i < count; ++i) {
        uint32_t& r = remap[ids[i]];
        if (r == kNever) r = nextId++;
        t.id[i] = r;
    }
    t.numObjects = nextId;
    t.computeDerived();
    return t;
}

// Converts "timestamp object size" text (space or tab separated) to a binary trace.
// The object key is (object, size): a changed size is a new version of the object.
// Requests with size 0 (no body, e.g. redirects) are dropped.
struct ConvertStats {
    uint64_t lines = 0, kept = 0, zeroSize = 0, malformed = 0;
};

inline Trace convertText(std::istream& in, ConvertStats& st) {
    struct KeyHash {
        std::size_t operator()(const std::pair<int64_t, uint32_t>& k) const {
            uint64_t x = static_cast<uint64_t>(k.first) * 0x9E3779B97F4A7C15ULL ^ (k.second + 0x632BE59BD9B4E019ULL);
            x ^= x >> 31;
            return static_cast<std::size_t>(x * 0xBF58476D1CE4E5B9ULL);
        }
    };
    std::unordered_map<std::pair<int64_t, uint32_t>, uint32_t, KeyHash> remap;
    remap.reserve(1 << 22);
    Trace t;
    std::string line;
    while (std::getline(in, line)) {
        ++st.lines;
        const char* p = line.c_str();
        char* end = nullptr;
        std::strtod(p, &end);  // timestamp (unused: time is logical)
        if (end == p) { ++st.malformed; continue; }
        p = end;
        const long long obj = std::strtoll(p, &end, 10);
        if (end == p) { ++st.malformed; continue; }
        p = end;
        const double sz = std::strtod(p, &end);
        if (end == p) { ++st.malformed; continue; }
        if (sz <= 0) { ++st.zeroSize; continue; }
        const uint32_t size = sz >= 4294967295.0 ? 4294967295u : static_cast<uint32_t>(sz);
        auto [it, inserted] = remap.emplace(std::make_pair(static_cast<int64_t>(obj), size),
                                            static_cast<uint32_t>(remap.size()));
        t.id.push_back(it->second);
        t.size.push_back(size);
        ++st.kept;
    }
    t.numObjects = static_cast<uint32_t>(remap.size());
    return t;
}

}  // namespace lab
