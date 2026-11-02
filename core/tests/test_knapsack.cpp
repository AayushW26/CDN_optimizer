#include "TestFramework.hpp"
#include "cdn/Knapsack.hpp"
#include "cdn/Random.hpp"

using namespace cdn;
using namespace cdn::knapsack;

namespace {

std::vector<Item> randomItems(Rng& rng, int n, int maxW, bool correlated = false) {
    std::vector<Item> v(static_cast<std::size_t>(n));
    for (auto& it : v) {
        it.weight = rng.intIn(1, maxW);
        it.value = correlated ? static_cast<double>(it.weight) + maxW / 10.0 : rng.uniform(0.0, 100.0);
    }
    return v;
}

void checkConsistent(const std::vector<Item>& items, const Result& r, i64 C) {
    double v;
    i64 w;
    evaluate(items, r.chosen, v, w);
    CHECK(w <= C);
    CHECK(w == r.weight);
    CHECK_NEAR(v, r.value, 1e-9);
    for (std::size_t k = 1; k < r.chosen.size(); ++k) CHECK(r.chosen[k - 1] < r.chosen[k]);  // no duplicates
}

}  // namespace

TEST(exact_solvers_match_brute_force) {
    Rng rng(1);
    for (int trial = 0; trial < 600; ++trial) {
        const int n = rng.intIn(0, 16);
        const bool corr = trial % 3 == 0;
        auto items = randomItems(rng, n, 30, corr);
        const i64 C = rng.intIn(0, 120);
        Result ref = bruteForce(items, C);
        for (const char* algo : {"dp2d", "rolling", "hirschberg", "bnb", "mitm"}) {
            Result r = solve(algo, items, C);
            checkConsistent(items, r, C);
            CHECK_NEAR(r.value, ref.value, 1e-9);
            CHECK(r.provenOptimal);
        }
    }
}

TEST(hirschberg_matches_dp_on_large_instances) {
    Rng rng(2);
    for (int trial = 0; trial < 20; ++trial) {
        auto items = randomItems(rng, rng.intIn(50, 400), 200, trial % 2 == 0);
        const i64 C = rng.intIn(100, 3000);
        Result a = dpRolling(items, C);
        Result b = dpHirschberg(items, C);
        Result c = branchAndBound(items, C);
        checkConsistent(items, b, C);
        CHECK_NEAR(a.value, b.value, 1e-9);
        if (c.provenOptimal) CHECK_NEAR(a.value, c.value, 1e-9);
        CHECK(b.memoryBytes < a.memoryBytes + 64 * 1024 || C < 64);
    }
}

TEST(fptas_respects_approximation_guarantee) {
    Rng rng(3);
    for (double eps : {0.5, 0.2, 0.1, 0.02}) {
        for (int trial = 0; trial < 60; ++trial) {
            auto items = randomItems(rng, rng.intIn(1, 60), 50, trial % 2 == 0);
            const i64 C = rng.intIn(1, 400);
            Result opt = dpRolling(items, C);
            Result ap = fptas(items, C, eps);
            checkConsistent(items, ap, C);
            CHECK(ap.value >= (1.0 - eps) * opt.value - 1e-9);
            CHECK(ap.value <= opt.value + 1e-9);
        }
    }
}

TEST(modified_greedy_is_half_approximation_and_bound_is_upper) {
    Rng rng(4);
    for (int trial = 0; trial < 300; ++trial) {
        auto items = randomItems(rng, rng.intIn(1, 40), 60, trial % 2 == 0);
        const i64 C = rng.intIn(1, 300);
        Result opt = dpRolling(items, C);
        Result mg = modifiedGreedy(items, C);
        Result g = greedyDensity(items, C);
        checkConsistent(items, mg, C);
        checkConsistent(items, g, C);
        CHECK(mg.value >= 0.5 * opt.value - 1e-9);
        CHECK(dantzigBound(items, C) >= opt.value - 1e-9);
    }
}

TEST(greedy_can_be_arbitrarily_bad_but_modified_greedy_is_not) {
    // classic counter-example: tiny dense item blocks a huge valuable one
    std::vector<Item> items = {{1, 2.0}, {100, 100.0}};
    Result g = greedyDensity(items, 100);
    Result mg = modifiedGreedy(items, 100);
    Result opt = dp2d(items, 100);
    CHECK_NEAR(g.value, 2.0, 1e-12);
    CHECK_NEAR(opt.value, 100.0, 1e-12);
    CHECK_NEAR(mg.value, 100.0, 1e-12);
}

TEST(edge_cases_zero_weight_and_oversized_items) {
    std::vector<Item> items = {{0, 5.0}, {10, 1.0}, {3, 0.0}, {2, 4.0}};
    for (const char* algo : {"dp2d", "rolling", "hirschberg", "bnb", "mitm", "brute", "greedy", "modgreedy", "fptas:0.1"}) {
        Result r = solve(algo, items, 2);
        checkConsistent(items, r, 2);
        CHECK_NEAR(r.value, 9.0, 1e-9);  // zero-weight item always taken + item 3
    }
    Result empty = dpRolling({}, 10);
    CHECK(empty.chosen.empty());
    Result zeroCap = dpHirschberg(items, 0);
    CHECK_NEAR(zeroCap.value, 5.0, 1e-12);
}

TEST(mitm_handles_huge_capacity) {
    Rng rng(5);
    std::vector<Item> items(30);
    for (auto& it : items) {
        it.weight = static_cast<i64>(rng.below(1'000'000'000ULL)) + 1;
        it.value = rng.uniform(1, 1000);
    }
    const i64 C = 7'000'000'000LL;  // a DP table would need 7e9 columns
    Result m = meetInMiddle(items, C);
    Result b = branchAndBound(items, C);
    checkConsistent(items, m, C);
    CHECK(b.provenOptimal);
    CHECK_NEAR(m.value, b.value, 1e-9);
}

TEST(mckp_matches_brute_force) {
    Rng rng(6);
    for (int trial = 0; trial < 300; ++trial) {
        std::vector<Group> groups(static_cast<std::size_t>(rng.intIn(0, 6)));
        for (auto& g : groups) {
            const int k = rng.intIn(1, 4);
            for (int j = 0; j < k; ++j) g.options.push_back({rng.intIn(1, 20), rng.uniform(0, 50)});
        }
        const i64 C = rng.intIn(0, 60);
        MckpResult a = mckp(groups, C);
        MckpResult b = mckpBruteForce(groups, C);
        CHECK_NEAR(a.value, b.value, 1e-9);
        CHECK(a.weight <= C);
        double v = 0;
        i64 w = 0;
        for (std::size_t g = 0; g < groups.size(); ++g)
            if (a.choice[g] >= 0) {
                v += groups[g].options[static_cast<std::size_t>(a.choice[g])].value;
                w += groups[g].options[static_cast<std::size_t>(a.choice[g])].weight;
            }
        CHECK_NEAR(v, a.value, 1e-9);
        CHECK(w == a.weight);
    }
}

TEST(auto_solver_is_exact_when_within_budget) {
    Rng rng(7);
    auto items = randomItems(rng, 300, 100);
    Result a = solveAuto(items, 2000);
    Result b = dpRolling(items, 2000);
    CHECK_NEAR(a.value, b.value, 1e-9);
}
