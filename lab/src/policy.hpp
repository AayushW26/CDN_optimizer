// Policy interface and shared building blocks for the cache lab.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "trace.hpp"

namespace lab {

// Every policy sees the same request stream. access() returns true on a hit.
// Policies own their capacity accounting; an object larger than the cache is never cached.
class Policy {
public:
    virtual ~Policy() = default;
    virtual std::string name() const = 0;
    // i = request index (logical time), obj = dense id, size = bytes
    virtual bool access(std::size_t i, uint32_t obj, uint32_t size) = 0;
    virtual uint64_t usedBytes() const = 0;
};

// Doubly linked list over dense object ids (intrusive, O(1) everything).
// head = most recent end, tail = oldest end.
class IdList {
public:
    explicit IdList(uint32_t n = 0) { resize(n); }
    void resize(uint32_t n) {
        prev_.assign(n, kNever);
        next_.assign(n, kNever);
    }
    bool empty() const { return head_ == kNever; }
    uint32_t head() const { return head_; }
    uint32_t tail() const { return tail_; }
    uint32_t prevOf(uint32_t x) const { return prev_[x]; }  // toward head (newer)
    uint32_t nextOf(uint32_t x) const { return next_[x]; }  // toward tail (older)
    std::size_t size() const { return count_; }

    void pushHead(uint32_t x) {
        prev_[x] = kNever;
        next_[x] = head_;
        if (head_ != kNever) prev_[head_] = x;
        head_ = x;
        if (tail_ == kNever) tail_ = x;
        ++count_;
    }
    void remove(uint32_t x) {
        const uint32_t p = prev_[x], q = next_[x];
        if (p != kNever) next_[p] = q; else head_ = q;
        if (q != kNever) prev_[q] = p; else tail_ = p;
        prev_[x] = next_[x] = kNever;
        --count_;
    }
    void moveToHead(uint32_t x) {
        if (head_ == x) return;
        remove(x);
        pushHead(x);
    }

private:
    std::vector<uint32_t> prev_, next_;
    uint32_t head_ = kNever, tail_ = kNever;
    std::size_t count_ = 0;
};

// Set of cached ids supporting O(1) insert/erase and uniform random sampling.
class SampleSet {
public:
    explicit SampleSet(uint32_t n = 0) : pos_(n, kNever) {}
    void resize(uint32_t n) { pos_.assign(n, kNever); }
    bool contains(uint32_t x) const { return pos_[x] != kNever; }
    void insert(uint32_t x) {
        pos_[x] = static_cast<uint32_t>(items_.size());
        items_.push_back(x);
    }
    void erase(uint32_t x) {
        const uint32_t p = pos_[x];
        const uint32_t last = items_.back();
        items_[p] = last;
        pos_[last] = p;
        items_.pop_back();
        pos_[x] = kNever;
    }
    std::size_t size() const { return items_.size(); }
    uint32_t at(std::size_t k) const { return items_[k]; }

private:
    std::vector<uint32_t> pos_;
    std::vector<uint32_t> items_;
};

// Small, fast, deterministic PRNG (splitmix64 seeded xorshift).
class Rng {
public:
    explicit Rng(uint64_t seed = 42) : s_(seed ? seed : 1) {}
    uint64_t next() {
        uint64_t z = (s_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }

private:
    uint64_t s_;
};

struct PolicyContext {
    const Trace* trace = nullptr;  // only offline references may look at trace->next
    uint64_t capacity = 0;         // bytes
    uint64_t seed = 42;
};

}  // namespace lab
