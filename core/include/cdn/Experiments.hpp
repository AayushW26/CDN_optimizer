// ExperimentRunner: the experiment matrix of the report. Every experiment
// writes one JSON file into the output directory; the dashboard reads them.
#pragma once

#include <string>
#include <vector>

namespace cdn {

struct ExperimentOptions {
    std::string outDir = "results";
    bool quick = false;  // smaller matrix for CI / smoke tests
    int threads = 0;
};

std::vector<std::string> experimentNames();
/// Run one experiment by name (or "all"). Returns the files written.
std::vector<std::string> runExperiment(const std::string& name, const ExperimentOptions& opt);

}  // namespace cdn
