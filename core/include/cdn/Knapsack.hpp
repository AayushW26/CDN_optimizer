// Knapsack solver library: the algorithmic core of the project.
//
// Every solver takes the same (items, capacity) instance and returns the same
// KnapsackResult shape, so they can be compared head-to-head for optimality,
// running time and memory (see `cdnopt experiment solvers`).
//
//   Solver                 Time            Space            Guarantee
//   ---------------------  --------------  ---------------  -----------------
//   dp2d                   O(nC)           O(nC) doubles    exact
//   dpRolling (+bitset)    O(nC)           O(C) + nC bits   exact
//   dpHirschberg (D&C)     O(nC)           O(n + C)         exact
//   branchAndBound         O(2^n) worst    O(n)             exact (node cap)
//   meetInMiddle           O(2^{n/2} n)    O(2^{n/2})       exact, C-independent
//   fptas(eps)             O(n^2 / eps)    O(n^2/eps) bits  >= (1-eps) OPT
//   greedyDensity          O(n log n)      O(n)             none
//   modifiedGreedy         O(n log n)      O(n)             >= OPT / 2
//   bruteForce             O(2^n n)        O(n)             exact (tests only)
//
//   mckp (multiple-choice) O(C * sum_g |g|) O(C + G*C bytes) exact
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cdn/Common.hpp"

namespace cdn::knapsack {

struct Item {
    i64 weight = 0;     // integer size units (must be >= 0)
    double value = 0;   // benefit (must be >= 0)
};

struct Result {
    std::string algorithm;
    double value = 0;            // sum of chosen values
    i64 weight = 0;              // sum of chosen weights
    std::vector<int> chosen;     // indices into the input vector, ascending
    double millis = 0;           // wall-clock solve time
    std::size_t memoryBytes = 0; // dominant table memory (analytic)
    bool provenOptimal = false;  // exact solver finished without hitting limits
    long long nodes = 0;         // B&B nodes explored (0 for others)
};

// ---- exact ---------------------------------------------------------------
Result dp2d(const std::vector<Item>& items, i64 capacity);
Result dpRolling(const std::vector<Item>& items, i64 capacity);
Result dpHirschberg(const std::vector<Item>& items, i64 capacity);
Result branchAndBound(const std::vector<Item>& items, i64 capacity,
                      long long nodeLimit = 50'000'000);
Result meetInMiddle(const std::vector<Item>& items, i64 capacity);  // n <= 44
Result bruteForce(const std::vector<Item>& items, i64 capacity);    // n <= 24

// ---- approximate ---------------------------------------------------------
Result fptas(const std::vector<Item>& items, i64 capacity, double eps);
Result greedyDensity(const std::vector<Item>& items, i64 capacity);
Result modifiedGreedy(const std::vector<Item>& items, i64 capacity);

/// Dantzig LP-relaxation upper bound (fractional knapsack). OPT <= bound.
double dantzigBound(const std::vector<Item>& items, i64 capacity);

/// Dispatch by name: dp2d | rolling | hirschberg | bnb | mitm | brute |
/// fptas:<eps> | greedy | modgreedy | auto
Result solve(const std::string& algorithm, const std::vector<Item>& items, i64 capacity);

/// "auto" policy used by the cache planners: exact Hirschberg DP when n*C fits
/// the work budget, otherwise B&B with a node cap, falling back to FPTAS.
Result solveAuto(const std::vector<Item>& items, i64 capacity, double workBudget = 6e7);

// ---- Multiple-Choice Knapsack (one option per group) ---------------------
struct Group {
    std::vector<Item> options;  // at most one may be chosen (or none)
};

struct MckpResult {
    double value = 0;
    i64 weight = 0;
    std::vector<int> choice;  // per group: chosen option index, or -1
    double millis = 0;
    std::size_t memoryBytes = 0;
};

MckpResult mckp(const std::vector<Group>& groups, i64 capacity);
MckpResult mckpBruteForce(const std::vector<Group>& groups, i64 capacity);  // tests

/// Recompute (value, weight) of a chosen index set -- used by tests.
void evaluate(const std::vector<Item>& items, const std::vector<int>& chosen, double& value,
              i64& weight);

}  // namespace cdn::knapsack
