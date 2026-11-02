// Tiny dependency-free unit-test framework (keeps the build hermetic).
#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace tf {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const std::string& n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

struct Failure {
    std::string msg;
};

inline int& checks() {
    static int c = 0;
    return c;
}

}  // namespace tf

#define TF_CAT2(a, b) a##b
#define TF_CAT(a, b) TF_CAT2(a, b)
#define TEST(name)                                                       \
    static void TF_CAT(tf_test_, name)();                                \
    static tf::Registrar TF_CAT(tf_reg_, name)(#name, TF_CAT(tf_test_, name)); \
    static void TF_CAT(tf_test_, name)()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++tf::checks();                                                                   \
        if (!(cond)) {                                                                    \
            std::ostringstream os_;                                                       \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed";            \
            throw tf::Failure{os_.str()};                                                 \
        }                                                                                 \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                    \
    do {                                                                                         \
        ++tf::checks();                                                                          \
        const double a_ = (a), b_ = (b);                                                         \
        if (std::fabs(a_ - b_) > (tol) * std::max(1.0, std::max(std::fabs(a_), std::fabs(b_)))) { \
            std::ostringstream os_;                                                              \
            os_ << __FILE__ << ":" << __LINE__ << ": CHECK_NEAR(" #a ", " #b ") " << a_          \
                << " vs " << b_;                                                                 \
            throw tf::Failure{os_.str()};                                                        \
        }                                                                                        \
    } while (0)
