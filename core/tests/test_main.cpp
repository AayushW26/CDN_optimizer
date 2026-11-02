#include <chrono>

#include "TestFramework.hpp"

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0, failed = 0;
    for (const auto& t : tf::registry()) {
        if (!filter.empty() && t.name.find(filter) == std::string::npos) continue;
        auto start = std::chrono::steady_clock::now();
        try {
            t.fn();
            ++passed;
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << "[ PASS ] " << t.name << " (" << ms << " ms)\n";
        } catch (const tf::Failure& f) {
            ++failed;
            std::cout << "[ FAIL ] " << t.name << "\n         " << f.msg << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[ FAIL ] " << t.name << "\n         exception: " << e.what() << "\n";
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << tf::checks() << " checks\n";
    return failed == 0 ? 0 : 1;
}
