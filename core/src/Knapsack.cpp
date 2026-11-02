#include "cdn/Knapsack.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <stdexcept>

namespace cdn::knapsack {
namespace {

constexpr double kEps = 1e-12;

/// Normalised view of an instance: zero-weight items are always taken,
/// items that cannot fit (w > C) or are worthless (v <= 0) are dropped.
struct Prepared {
    std::vector<int> active;  // indices into the original vector
    std::vector<int> forced;  // zero-weight, positive-value items
};

Prepared prepare(const std::vector<Item>& items, i64 capacity) {
    if (capacity < 0) throw std::invalid_argument("knapsack: negative capacity");
    Prepared p;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const Item& it = items[i];
        if (it.weight < 0 || it.value < 0) throw std::invalid_argument("knapsack: negative item");
        if (it.value <= 0) continue;
        if (it.weight == 0) {
            p.forced.push_back(i);
        } else if (it.weight <= capacity) {
            p.active.push_back(i);
        }
    }
    return p;
}

void finish(Result& r, const std::vector<Item>& items, const Prepared& p) {
    for (int f : p.forced) r.chosen.push_back(f);
    std::sort(r.chosen.begin(), r.chosen.end());
    evaluate(items, r.chosen, r.value, r.weight);
}

/// Simple dynamically sized bitset for keep-tables.
class BitTable {
public:
    BitTable(std::size_t rows, std::size_t cols) : cols_(cols), words_((rows * cols + 63) / 64, 0) {}
    void set(std::size_t r, std::size_t c) {
        std::size_t k = r * cols_ + c;
        words_[k >> 6] |= (1ULL << (k & 63));
    }
    bool get(std::size_t r, std::size_t c) const {
        std::size_t k = r * cols_ + c;
        return (words_[k >> 6] >> (k & 63)) & 1ULL;
    }
    std::size_t bytes() const { return words_.size() * sizeof(u64); }

private:
    std::size_t cols_;
    std::vector<u64> words_;
};

std::vector<int> byDensity(const std::vector<Item>& items, const std::vector<int>& idx) {
    std::vector<int> order = idx;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        // a/wa > b/wb  <=>  a*wb > b*wa  (weights are positive here)
        double lhs = items[a].value * static_cast<double>(items[b].weight);
        double rhs = items[b].value * static_cast<double>(items[a].weight);
        if (lhs != rhs) return lhs > rhs;
        return a < b;
    });
    return order;
}

/// Forward rolling DP over items[idx[lo..hi)] -> F[c] = best value with weight <= c.
void forwardPass(const std::vector<Item>& items, const std::vector<int>& idx, std::size_t lo,
                 std::size_t hi, i64 C, std::vector<double>& F) {
    F.assign(static_cast<std::size_t>(C) + 1, 0.0);
    for (std::size_t k = lo; k < hi; ++k) {
        const Item& it = items[idx[k]];
        if (it.weight > C) continue;
        const double v = it.value;
        for (i64 c = C; c >= it.weight; --c) {
            double cand = F[c - it.weight] + v;
            if (cand > F[c]) F[c] = cand;
        }
    }
}

/// Small exact DP with bit keep-table, used at the leaves of Hirschberg recursion.
void leafSolve(const std::vector<Item>& items, const std::vector<int>& idx, std::size_t lo,
               std::size_t hi, i64 C, std::vector<int>& out) {
    const std::size_t m = hi - lo;
    std::vector<double> dp(static_cast<std::size_t>(C) + 1, 0.0);
    BitTable keep(m, static_cast<std::size_t>(C) + 1);
    for (std::size_t k = 0; k < m; ++k) {
        const Item& it = items[idx[lo + k]];
        if (it.weight > C) continue;
        for (i64 c = C; c >= it.weight; --c) {
            double cand = dp[c - it.weight] + it.value;
            if (cand > dp[c]) {
                dp[c] = cand;
                keep.set(k, c);
            }
        }
    }
    i64 c = C;
    for (std::size_t k = m; k-- > 0;) {
        if (keep.get(k, c)) {
            out.push_back(idx[lo + k]);
            c -= items[idx[lo + k]].weight;
        }
    }
}

void hirschbergRec(const std::vector<Item>& items, const std::vector<int>& idx, std::size_t lo,
                   std::size_t hi, i64 C, std::vector<int>& out, std::size_t& peak) {
    if (lo >= hi || C <= 0) return;
    if (hi - lo <= 16) {  // leaf: (hi-lo)*(C+1) bits is tiny
        leafSolve(items, idx, lo, hi, C, out);
        return;
    }
    const std::size_t mid = lo + (hi - lo) / 2;
    i64 split = 0;
    {
        std::vector<double> F, G;
        forwardPass(items, idx, lo, mid, C, F);
        forwardPass(items, idx, mid, hi, C, G);
        peak = std::max(peak, 2 * (static_cast<std::size_t>(C) + 1) * sizeof(double));
        double best = -1;
        for (i64 c = 0; c <= C; ++c) {
            double s = F[c] + G[C - c];
            if (s > best) {
                best = s;
                split = c;
            }
        }
    }  // F, G released before recursing -> O(C) live memory
    hirschbergRec(items, idx, lo, mid, split, out, peak);
    hirschbergRec(items, idx, mid, hi, C - split, out, peak);
}

}  // namespace

void evaluate(const std::vector<Item>& items, const std::vector<int>& chosen, double& value,
              i64& weight) {
    value = 0;
    weight = 0;
    for (int i : chosen) {
        value += items[i].value;
        weight += items[i].weight;
    }
}

// --------------------------------------------------------------------------
Result dp2d(const std::vector<Item>& items, i64 C) {
    Stopwatch sw;
    Result r;
    r.algorithm = "dp2d";
    Prepared p = prepare(items, C);
    const std::size_t n = p.active.size();
    const std::size_t cols = static_cast<std::size_t>(C) + 1;
    const double bytes = static_cast<double>(n + 1) * cols * sizeof(double);
    if (bytes > 3.0e9) throw std::length_error("dp2d: table too large");
    // dp[i][c] = best value using the first i active items with capacity c
    std::vector<double> dp((n + 1) * cols, 0.0);
    for (std::size_t i = 1; i <= n; ++i) {
        const Item& it = items[p.active[i - 1]];
        const double* prev = &dp[(i - 1) * cols];
        double* cur = &dp[i * cols];
        for (std::size_t c = 0; c < cols; ++c) {
            cur[c] = prev[c];
            if (static_cast<i64>(c) >= it.weight) {
                double cand = prev[c - it.weight] + it.value;
                if (cand > cur[c]) cur[c] = cand;
            }
        }
    }
    std::size_t c = static_cast<std::size_t>(C);
    for (std::size_t i = n; i >= 1; --i) {
        if (dp[i * cols + c] > dp[(i - 1) * cols + c]) {
            r.chosen.push_back(p.active[i - 1]);
            c -= items[p.active[i - 1]].weight;
        }
    }
    r.memoryBytes = static_cast<std::size_t>(bytes);
    r.provenOptimal = true;
    finish(r, items, p);
    r.millis = sw.ms();
    return r;
}

Result dpRolling(const std::vector<Item>& items, i64 C) {
    Stopwatch sw;
    Result r;
    r.algorithm = "rolling";
    Prepared p = prepare(items, C);
    const std::size_t n = p.active.size();
    const std::size_t cols = static_cast<std::size_t>(C) + 1;
    if (static_cast<double>(n) * cols > 6.4e10) throw std::length_error("rolling: keep table too large");
    std::vector<double> dp(cols, 0.0);
    BitTable keep(n, cols);
    for (std::size_t i = 0; i < n; ++i) {
        const Item& it = items[p.active[i]];
        for (i64 c = C; c >= it.weight; --c) {  // downward: reuse row i-1 values
            double cand = dp[c - it.weight] + it.value;
            if (cand > dp[c]) {
                dp[c] = cand;
                keep.set(i, c);
            }
        }
    }
    i64 c = C;
    for (std::size_t i = n; i-- > 0;) {
        if (keep.get(i, c)) {
            r.chosen.push_back(p.active[i]);
            c -= items[p.active[i]].weight;
        }
    }
    r.memoryBytes = cols * sizeof(double) + keep.bytes();
    r.provenOptimal = true;
    finish(r, items, p);
    r.millis = sw.ms();
    return r;
}

Result dpHirschberg(const std::vector<Item>& items, i64 C) {
    Stopwatch sw;
    Result r;
    r.algorithm = "hirschberg";
    Prepared p = prepare(items, C);
    std::size_t peak = 0;
    hirschbergRec(items, p.active, 0, p.active.size(), C, r.chosen, peak);
    r.memoryBytes = peak + p.active.size() * sizeof(int);
    r.provenOptimal = true;
    finish(r, items, p);
    r.millis = sw.ms();
    return r;
}

// --------------------------------------------------------------------------
double dantzigBound(const std::vector<Item>& items, i64 C) {
    Prepared p = prepare(items, C);
    double v = 0;
    for (int f : p.forced) v += items[f].value;
    i64 room = C;
    for (int i : byDensity(items, p.active)) {
        if (items[i].weight <= room) {
            room -= items[i].weight;
            v += items[i].value;
        } else {
            v += items[i].value * static_cast<double>(room) / static_cast<double>(items[i].weight);
            break;
        }
    }
    return v;
}

Result greedyDensity(const std::vector<Item>& items, i64 C) {
    Stopwatch sw;
    Result r;
    r.algorithm = "greedy";
    Prepared p = prepare(items, C);
    i64 room = C;
    for (int i : byDensity(items, p.active)) {
        if (items[i].weight <= room) {
            room -= items[i].weight;
            r.chosen.push_back(i);
        }
    }
    r.memoryBytes = p.active.size() * sizeof(int);
    finish(r, items, p);
    r.millis = sw.ms();
    return r;
}

Result modifiedGreedy(const std::vector<Item>& items, i64 C) {
    Stopwatch sw;
    Result g = greedyDensity(items, C);
    Prepared p = prepare(items, C);
    int bestSingle = -1;
    for (int i : p.active)
        if (bestSingle < 0 || items[i].value > items[bestSingle].value) bestSingle = i;
    Result r;
    r.algorithm = "modgreedy";
    if (bestSingle >= 0) {
        double forcedV = 0;
        for (int f : p.forced) forcedV += items[f].value;
        if (items[bestSingle].value + forcedV > g.value) {
            r.chosen = {bestSingle};
            finish(r, items, p);
            r.memoryBytes = g.memoryBytes;
            r.millis = sw.ms();
            return r;
        }
    }
    g.algorithm = "modgreedy";
    g.millis = sw.ms();
    return g;
}

// --------------------------------------------------------------------------
Result branchAndBound(const std::vector<Item>& items, i64 C, long long nodeLimit) {
    Stopwatch sw;
    Result r;
    r.algorithm = "bnb";
    Prepared p = prepare(items, C);
    const std::vector<int> order = byDensity(items, p.active);
    const std::size_t n = order.size();
    std::vector<i64> W(n + 1, 0);
    std::vector<double> V(n + 1, 0.0);
    for (std::size_t k = 0; k < n; ++k) {
        W[k + 1] = W[k] + items[order[k]].weight;
        V[k + 1] = V[k] + items[order[k]].value;
    }
    auto density = [&](std::size_t j) {
        return items[order[j]].value / static_cast<double>(items[order[j]].weight);
    };
    // Martello-Toth U2 bound of the sub-problem (items k..n-1, residual capacity room).
    // With break item j (first that does not fit) and residual r:
    //   U0 = prefix + r * d_{j+1}                     (item j excluded)
    //   U1 = prefix + v_j - (w_j - r) * d_{j-1}       (item j forced in)
    //   U2 = max(U0, U1)  <=  Dantzig bound prefix + r * d_j
    auto bound = [&](std::size_t k, i64 room) {
        const i64 target = W[k] + room;  // largest j with W[j] - W[k] <= room
        const std::size_t j =
            static_cast<std::size_t>(std::upper_bound(W.begin() + static_cast<std::ptrdiff_t>(k), W.end(), target) - W.begin()) - 1;
        const double prefix = V[j] - V[k];
        if (j >= n) return prefix;
        const double r = static_cast<double>(target - W[j]);
        const double u0 = prefix + (j + 1 < n ? r * density(j + 1) : 0.0);
        double u1 = -1e300;
        if (j > k) u1 = prefix + items[order[j]].value - (static_cast<double>(items[order[j]].weight) - r) * density(j - 1);
        return std::max(u0, u1);
    };

    // Incumbent from greedy.
    Result g = greedyDensity(items, C);
    double best = 0;
    std::vector<int> bestSet;
    for (int i : g.chosen) {
        if (items[i].weight > 0) {
            best += items[i].value;
            bestSet.push_back(i);
        }
    }
    std::vector<int> path;
    path.reserve(n);
    long long nodes = 0;
    bool aborted = false;

    std::function<void(std::size_t, i64, double)> dfs = [&](std::size_t k, i64 room, double val) {
        if (aborted) return;
        if (++nodes > nodeLimit) {
            aborted = true;
            return;
        }
        if (val > best + kEps) {
            best = val;
            bestSet.assign(path.begin(), path.end());
        }
        if (k == n) return;
        if (val + bound(k, room) <= best * (1 + 1e-10) + kEps) return;  // prune (1e-10 relative tolerance)
        const Item& it = items[order[k]];
        if (it.weight <= room) {
            path.push_back(order[k]);
            dfs(k + 1, room - it.weight, val + it.value);
            path.pop_back();
        }
        dfs(k + 1, room, val);
    };
    dfs(0, C, 0.0);

    r.chosen = bestSet;
    r.nodes = nodes;
    r.provenOptimal = !aborted;
    r.memoryBytes = n * (sizeof(i64) + sizeof(double) + 2 * sizeof(int));
    finish(r, items, p);
    r.millis = sw.ms();
    return r;
}

// --------------------------------------------------------------------------
Result meetInMiddle(const std::vector<Item>& items, i64 C) {
    Stopwatch sw;
    Result r;
    r.algorithm = "mitm";
    Prepared p = prepare(items, C);
    const std::size_t n = p.active.size();
    if (n > 44) throw std::length_error("mitm: too many items (n > 44)");
    const std::size_t h = n / 2;
    struct Sub {
        i64 w;
        double v;
        u64 mask;
    };
    auto enumerate = [&](std::size_t lo, std::size_t hi) {
        const std::size_t m = hi - lo;
        std::vector<Sub> out;
        out.reserve(std::size_t{1} << m);
        out.push_back({0, 0.0, 0});
        for (std::size_t k = 0; k < m; ++k) {  // iterative doubling
            const Item& it = items[p.active[lo + k]];
            const std::size_t sz = out.size();
            for (std::size_t s = 0; s < sz; ++s) {
                if (out[s].w + it.weight <= C)
                    out.push_back({out[s].w + it.weight, out[s].v + it.value, out[s].mask | (1ULL << k)});
            }
        }
        return out;
    };
    std::vector<Sub> A = enumerate(0, h);
    std::vector<Sub> B = enumerate(h, n);
    std::sort(B.begin(), B.end(), [](const Sub& a, const Sub& b) { return a.w < b.w; });
    // prefix-max over B by weight
    std::vector<std::size_t> bestUpTo(B.size());
    for (std::size_t i = 0; i < B.size(); ++i)
        bestUpTo[i] = (i == 0 || B[i].v > B[bestUpTo[i - 1]].v) ? i : bestUpTo[i - 1];
    double best = -1;
    u64 ma = 0, mb = 0;
    for (const Sub& a : A) {
        i64 room = C - a.w;
        auto it = std::upper_bound(B.begin(), B.end(), room, [](i64 x, const Sub& s) { return x < s.w; });
        if (it == B.begin()) continue;
        std::size_t j = bestUpTo[static_cast<std::size_t>(it - B.begin()) - 1];
        if (a.v + B[j].v > best) {
            best = a.v + B[j].v;
            ma = a.mask;
            mb = B[j].mask;
        }
    }
    for (std::size_t k = 0; k < h; ++k)
        if (ma >> k & 1ULL) r.chosen.push_back(p.active[k]);
    for (std::size_t k = 0; k < n - h; ++k)
        if (mb >> k & 1ULL) r.chosen.push_back(p.active[h + k]);
    r.memoryBytes = (A.capacity() + B.capacity()) * sizeof(Sub) + bestUpTo.size() * sizeof(std::size_t);
    r.provenOptimal = true;
    finish(r, items, p);
    r.millis = sw.ms();
    return r;
}

Result bruteForce(const std::vector<Item>& items, i64 C) {
    Stopwatch sw;
    Result r;
    r.algorithm = "brute";
    Prepared p = prepare(items, C);
    const std::size_t n = p.active.size();
    if (n > 24) throw std::length_error("brute: n > 24");
    double best = 0;
    u64 bestMask = 0;
    for (u64 mask = 0; mask < (1ULL << n); ++mask) {
        i64 w = 0;
        double v = 0;
        for (std::size_t k = 0; k < n; ++k)
            if (mask >> k & 1ULL) {
                w += items[p.active[k]].weight;
                v += items[p.active[k]].value;
            }
        if (w <= C && v > best) {
            best = v;
            bestMask = mask;
        }
    }
    for (std::size_t k = 0; k < n; ++k)
        if (bestMask >> k & 1ULL) r.chosen.push_back(p.active[k]);
    r.provenOptimal = true;
    finish(r, items, p);
    r.millis = sw.ms();
    return r;
}

// --------------------------------------------------------------------------
// FPTAS by profit scaling (Ibarra-Kim / Lawler style).
//   LB = modified greedy value  (OPT/2 <= LB <= OPT)
//   K  = eps * LB / n,  p_i = floor(v_i / K)
//   DP over scaled profit: minW[q] = min weight reaching scaled profit q.
//   Any feasible set has scaled profit <= OPT/K <= 2n/eps, so the table has
//   O(n/eps) columns -> O(n^2/eps) time, independent of the capacity C.
//   Rounding loses < K per chosen item  =>  value >= OPT - nK >= (1-eps) OPT.
Result fptas(const std::vector<Item>& items, i64 C, double eps) {
    Stopwatch sw;
    if (!(eps > 0 && eps < 1)) throw std::invalid_argument("fptas: eps must be in (0,1)");
    Result r;
    r.algorithm = "fptas";
    Prepared p = prepare(items, C);
    const std::size_t n = p.active.size();
    Result lbRes = modifiedGreedy(items, C);
    double forcedV = 0;
    for (int f : p.forced) forcedV += items[f].value;
    const double LB = lbRes.value - forcedV;
    if (n == 0 || LB <= 0) {
        finish(r, items, p);
        r.millis = sw.ms();
        return r;
    }
    const double K = eps * LB / static_cast<double>(n);
    std::vector<i64> prof(n);
    i64 sumP = 0;
    for (std::size_t k = 0; k < n; ++k) {
        prof[k] = static_cast<i64>(std::floor(items[p.active[k]].value / K));
        sumP += prof[k];
    }
    const i64 P = std::min<i64>(sumP, static_cast<i64>(std::floor(2.0 * LB / K)) + 1);
    const std::size_t cols = static_cast<std::size_t>(P) + 1;
    if (static_cast<double>(n) * cols > 6.4e10) throw std::length_error("fptas: table too large");
    constexpr i64 INF = std::numeric_limits<i64>::max() / 4;
    std::vector<i64> minW(cols, INF);
    minW[0] = 0;
    BitTable keep(n, cols);
    for (std::size_t k = 0; k < n; ++k) {
        const i64 pk = std::min(prof[k], P);
        const i64 wk = items[p.active[k]].weight;
        if (pk == 0) continue;  // contributes < K; ignored by the scaled problem
        for (i64 q = P; q >= pk; --q) {
            if (minW[q - pk] == INF) continue;
            i64 cand = minW[q - pk] + wk;
            if (cand < minW[q]) {
                minW[q] = cand;
                keep.set(k, q);
            }
        }
    }
    i64 q = 0;
    for (i64 t = P; t >= 0; --t)
        if (minW[t] <= C) {
            q = t;
            break;
        }
    for (std::size_t k = n; k-- > 0 && q > 0;) {
        if (keep.get(k, q)) {
            r.chosen.push_back(p.active[k]);
            q -= std::min(prof[k], P);
        }
    }
    // Fill any residual capacity greedily (never hurts the guarantee).
    {
        i64 used = 0;
        std::vector<char> in(items.size(), 0);
        for (int i : r.chosen) {
            used += items[i].weight;
            in[i] = 1;
        }
        for (int i : byDensity(items, p.active))
            if (!in[i] && used + items[i].weight <= C) {
                used += items[i].weight;
                r.chosen.push_back(i);
            }
    }
    r.memoryBytes = cols * sizeof(i64) + keep.bytes();
    finish(r, items, p);
    if (r.value < lbRes.value) {  // never worse than the 1/2-approx incumbent
        lbRes.algorithm = "fptas";
        lbRes.millis = sw.ms();
        lbRes.memoryBytes = r.memoryBytes;
        return lbRes;
    }
    r.millis = sw.ms();
    return r;
}

// --------------------------------------------------------------------------
Result solveAuto(const std::vector<Item>& items, i64 C, double workBudget) {
    Stopwatch sw;
    Prepared p = prepare(items, C);
    const double work = static_cast<double>(p.active.size()) * static_cast<double>(C + 1);
    Result r;
    if (work <= workBudget) {
        r = dpRolling(items, C);
    } else if (work <= 8 * workBudget) {
        r = dpHirschberg(items, C);
    } else {
        r = branchAndBound(items, C, 2'000'000);
        if (!r.provenOptimal) {
            try {
                Result f = fptas(items, C, 0.02);
                if (f.value > r.value) r = f;
            } catch (const std::length_error&) {
            }
        }
    }
    r.algorithm = "auto:" + r.algorithm;
    r.millis = sw.ms();
    return r;
}

Result solve(const std::string& algo, const std::vector<Item>& items, i64 C) {
    if (algo == "dp2d") return dp2d(items, C);
    if (algo == "rolling") return dpRolling(items, C);
    if (algo == "hirschberg") return dpHirschberg(items, C);
    if (algo == "bnb") return branchAndBound(items, C);
    if (algo == "mitm") return meetInMiddle(items, C);
    if (algo == "brute") return bruteForce(items, C);
    if (algo == "greedy") return greedyDensity(items, C);
    if (algo == "modgreedy") return modifiedGreedy(items, C);
    if (algo == "auto") return solveAuto(items, C);
    if (algo.rfind("fptas", 0) == 0) {
        double eps = 0.1;
        auto pos = algo.find(':');
        if (pos != std::string::npos) eps = std::stod(algo.substr(pos + 1));
        Result r = fptas(items, C, eps);
        r.algorithm = algo;
        return r;
    }
    throw std::invalid_argument("unknown knapsack algorithm: " + algo);
}

// --------------------------------------------------------------------------
MckpResult mckp(const std::vector<Group>& groups, i64 C) {
    Stopwatch sw;
    if (C < 0) throw std::invalid_argument("mckp: negative capacity");
    const std::size_t G = groups.size();
    const std::size_t cols = static_cast<std::size_t>(C) + 1;
    if (static_cast<double>(G) * cols * 2 > 4e9) throw std::length_error("mckp: table too large");
    std::vector<double> dp(cols, 0.0), next(cols);
    // choice[g][c] = 1 + option index that produced next[c], or 0 for "none"
    std::vector<std::uint16_t> choice(G * cols, 0);
    for (std::size_t g = 0; g < G; ++g) {
        if (groups[g].options.size() >= 65535) throw std::length_error("mckp: group too large");
        next = dp;
        std::uint16_t* ch = &choice[g * cols];
        for (std::size_t j = 0; j < groups[g].options.size(); ++j) {
            const Item& o = groups[g].options[j];
            if (o.value <= 0 || o.weight > C || o.weight < 0) continue;
            for (i64 c = C; c >= o.weight; --c) {
                double cand = dp[c - o.weight] + o.value;
                if (cand > next[c]) {
                    next[c] = cand;
                    ch[c] = static_cast<std::uint16_t>(j + 1);
                }
            }
        }
        dp.swap(next);
    }
    MckpResult r;
    r.choice.assign(G, -1);
    i64 c = C;
    for (std::size_t g = G; g-- > 0;) {
        std::uint16_t j = choice[g * cols + c];
        if (j) {
            r.choice[g] = j - 1;
            c -= groups[g].options[j - 1].weight;
            r.value += groups[g].options[j - 1].value;
            r.weight += groups[g].options[j - 1].weight;
        }
    }
    r.memoryBytes = 2 * cols * sizeof(double) + choice.size() * sizeof(std::uint16_t);
    r.millis = sw.ms();
    return r;
}

MckpResult mckpBruteForce(const std::vector<Group>& groups, i64 C) {
    MckpResult best;
    best.choice.assign(groups.size(), -1);
    std::vector<int> cur(groups.size(), -1);
    std::function<void(std::size_t, i64, double)> rec = [&](std::size_t g, i64 w, double v) {
        if (g == groups.size()) {
            if (v > best.value) {
                best.value = v;
                best.weight = w;
                best.choice = cur;
            }
            return;
        }
        cur[g] = -1;
        rec(g + 1, w, v);
        for (std::size_t j = 0; j < groups[g].options.size(); ++j) {
            const Item& o = groups[g].options[j];
            if (w + o.weight <= C) {
                cur[g] = static_cast<int>(j);
                rec(g + 1, w + o.weight, v + o.value);
            }
        }
        cur[g] = -1;
    };
    rec(0, 0, 0.0);
    return best;
}

}  // namespace cdn::knapsack
