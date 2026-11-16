// Classic baselines: LRU, FIFO, SIEVE, S3-FIFO, GDSF.
#pragma once

#include <algorithm>
#include <queue>

#include "policy.hpp"

namespace lab {

// ---------------------------------------------------------------- LRU / FIFO
class LruPolicy : public Policy {
public:
    LruPolicy(const PolicyContext& c, bool fifo)
        : cap_(c.capacity), fifo_(fifo), list_(c.trace->numObjects), in_(c.trace->numObjects, 0),
          sz_(c.trace->numObjects, 0) {}
    std::string name() const override { return fifo_ ? "FIFO" : "LRU"; }
    uint64_t usedBytes() const override { return used_; }
    bool access(std::size_t, uint32_t x, uint32_t size) override {
        if (in_[x]) {
            if (!fifo_) list_.moveToHead(x);
            return true;
        }
        if (size > cap_) return false;
        while (used_ + size > cap_) {
            const uint32_t v = list_.tail();
            list_.remove(v);
            in_[v] = 0;
            used_ -= sz_[v];
        }
        list_.pushHead(x);
        in_[x] = 1;
        sz_[x] = size;
        used_ += size;
        return false;
    }

private:
    uint64_t cap_, used_ = 0;
    bool fifo_;
    IdList list_;
    std::vector<char> in_;
    std::vector<uint32_t> sz_;
};

// ---------------------------------------------------------------- SIEVE (Zhang et al., NSDI'24)
// FIFO queue + one "visited" bit; the hand walks from the oldest object toward newer
// ones, clearing visited bits and evicting the first unvisited object it finds.
class SievePolicy : public Policy {
public:
    explicit SievePolicy(const PolicyContext& c)
        : cap_(c.capacity), list_(c.trace->numObjects), in_(c.trace->numObjects, 0),
          visited_(c.trace->numObjects, 0), sz_(c.trace->numObjects, 0) {}
    std::string name() const override { return "SIEVE"; }
    uint64_t usedBytes() const override { return used_; }
    bool access(std::size_t, uint32_t x, uint32_t size) override {
        if (in_[x]) {
            visited_[x] = 1;
            return true;
        }
        if (size > cap_) return false;
        while (used_ + size > cap_) evictOne();
        list_.pushHead(x);
        in_[x] = 1;
        visited_[x] = 0;
        sz_[x] = size;
        used_ += size;
        return false;
    }

private:
    void evictOne() {
        uint32_t p = hand_ == kNever ? list_.tail() : hand_;
        while (visited_[p]) {
            visited_[p] = 0;
            p = list_.prevOf(p);
            if (p == kNever) p = list_.tail();
        }
        hand_ = list_.prevOf(p);
        list_.remove(p);
        in_[p] = 0;
        used_ -= sz_[p];
    }
    uint64_t cap_, used_ = 0;
    IdList list_;
    std::vector<char> in_, visited_;
    std::vector<uint32_t> sz_;
    uint32_t hand_ = kNever;
};

// ---------------------------------------------------------------- S3-FIFO (Yang et al., SOSP'23)
// Small FIFO (10% of bytes) filters one-hit wonders, main FIFO with 2-bit CLOCK-like
// reinsertion, ghost FIFO of recently evicted ids (90% of bytes worth of ids).
// Mirrors libCacheSim's S3FIFO defaults: move-to-main-threshold=2, ghost-size-ratio=0.9.
class S3FifoPolicy : public Policy {
public:
    explicit S3FifoPolicy(const PolicyContext& c)
        : cap_(c.capacity), smallCap_(static_cast<uint64_t>(c.capacity * 0.10)),
          mainCap_(c.capacity - static_cast<uint64_t>(c.capacity * 0.10)),
          ghostCap_(static_cast<uint64_t>(c.capacity * 0.90)), small_(c.trace->numObjects),
          main_(c.trace->numObjects), where_(c.trace->numObjects, 0), freq_(c.trace->numObjects, 0),
          sz_(c.trace->numObjects, 0), ghostSeq_(c.trace->numObjects, 0) {}
    std::string name() const override { return "S3-FIFO"; }
    uint64_t usedBytes() const override { return smallUsed_ + mainUsed_; }

    bool access(std::size_t, uint32_t x, uint32_t size) override {
        if (where_[x] != 0) {
            if (freq_[x] < 255) ++freq_[x];
            return true;
        }
        bool hitGhost = false;
        if (ghostSeq_[x] != 0) {  // remove from ghost
            ghostSeq_[x] = 0;
            ghostUsed_ -= ghostSize(x);
            hitGhost = true;
        }
        if (size > cap_) return false;
        if (!hitGhost && size >= smallCap_) return false;  // libCacheSim: too big for the small queue
        while (smallUsed_ + mainUsed_ + size > cap_) evict();
        sz_[x] = size;
        freq_[x] = 0;
        if (hitGhost || (!hasEvicted_ && smallUsed_ >= smallCap_)) {
            main_.pushHead(x);
            where_[x] = 2;
            mainUsed_ += size;
        } else {
            small_.pushHead(x);
            where_[x] = 1;
            smallUsed_ += size;
        }
        return false;
    }

private:
    uint64_t ghostSize(uint32_t x) const { return sz_[x]; }
    void evict() {
        hasEvicted_ = true;
        if (mainUsed_ > mainCap_ || smallUsed_ == 0) evictMain();
        else evictSmall();
    }
    void evictSmall() {
        while (smallUsed_ > 0) {
            const uint32_t t = small_.tail();
            small_.remove(t);
            smallUsed_ -= sz_[t];
            if (freq_[t] >= 2) {
                main_.pushHead(t);
                where_[t] = 2;
                mainUsed_ += sz_[t];
                freq_[t] = 0;
            } else {
                where_[t] = 0;
                ghostInsert(t);
                return;
            }
        }
    }
    void evictMain() {
        while (mainUsed_ > 0) {
            const uint32_t t = main_.tail();
            if (freq_[t] >= 1) {
                main_.moveToHead(t);
                freq_[t] = static_cast<uint8_t>(std::min<int>(freq_[t], 3) - 1);
            } else {
                main_.remove(t);
                mainUsed_ -= sz_[t];
                where_[t] = 0;
                return;
            }
        }
    }
    void ghostInsert(uint32_t x) {
        ghostSeq_[x] = ++seq_;
        ghostUsed_ += sz_[x];
        ghost_.push({x, seq_});
        while (ghostUsed_ > ghostCap_ && !ghost_.empty()) {
            auto [g, s] = ghost_.front();
            ghost_.pop();
            if (ghostSeq_[g] == s) {
                ghostSeq_[g] = 0;
                ghostUsed_ -= sz_[g];
            }
        }
    }
    uint64_t cap_, smallCap_, mainCap_, ghostCap_;
    uint64_t smallUsed_ = 0, mainUsed_ = 0, ghostUsed_ = 0;
    IdList small_, main_;
    std::vector<uint8_t> where_;  // 0 none, 1 small, 2 main
    std::vector<uint8_t> freq_;
    std::vector<uint32_t> sz_;
    std::vector<uint64_t> ghostSeq_;
    std::queue<std::pair<uint32_t, uint64_t>> ghost_;
    uint64_t seq_ = 0;
    bool hasEvicted_ = false;
};

// ---------------------------------------------------------------- GDSF (Cherkasova 1998)
// priority = L + frequency * cost / size, with cost = 1 (optimises object hit ratio).
// L ("inflation") is set to the priority of the last evicted object.
class GdsfPolicy : public Policy {
public:
    explicit GdsfPolicy(const PolicyContext& c)
        : cap_(c.capacity), in_(c.trace->numObjects, 0), freq_(c.trace->numObjects, 0),
          ver_(c.trace->numObjects, 0), sz_(c.trace->numObjects, 0) {}
    std::string name() const override { return "GDSF"; }
    uint64_t usedBytes() const override { return used_; }
    bool access(std::size_t, uint32_t x, uint32_t size) override {
        if (in_[x]) {
            ++freq_[x];
            push(x);
            return true;
        }
        if (size > cap_) return false;
        while (used_ + size > cap_) {
            const Entry e = heap_.top();
            heap_.pop();
            if (!in_[e.id] || ver_[e.id] != e.ver) continue;
            L_ = e.pri;
            in_[e.id] = 0;
            used_ -= sz_[e.id];
        }
        in_[x] = 1;
        freq_[x] = 1;
        sz_[x] = size;
        used_ += size;
        push(x);
        if (heap_.size() > 4 * (cachedCount() + 1024)) compact();
        return false;
    }

private:
    struct Entry {
        double pri;
        uint32_t id, ver;
        bool operator<(const Entry& o) const { return pri > o.pri; }  // min-heap
    };
    void push(uint32_t x) { heap_.push({L_ + static_cast<double>(freq_[x]) / sz_[x], x, ++ver_[x]}); }
    std::size_t cachedCount() const { return liveEstimate_; }
    void compact() {
        std::priority_queue<Entry> fresh;
        std::size_t live = 0;
        while (!heap_.empty()) {
            const Entry e = heap_.top();
            heap_.pop();
            if (in_[e.id] && ver_[e.id] == e.ver) {
                fresh.push(e);
                ++live;
            }
        }
        heap_.swap(fresh);
        liveEstimate_ = live;
    }
    uint64_t cap_, used_ = 0;
    double L_ = 0;
    std::vector<char> in_;
    std::vector<uint32_t> freq_, ver_, sz_;
    std::priority_queue<Entry> heap_;
    std::size_t liveEstimate_ = 1 << 16;
};

}  // namespace lab
