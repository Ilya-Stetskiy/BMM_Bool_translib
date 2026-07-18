#include <catch2/catch_test_macros.hpp>
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <map>

#include "bdd/bdd_to_aig.hpp"
#include "bdd/bdd_to_anf.hpp"
#include "bdd/bdd_to_thr.hpp"
#include "bdd/bdd_to_tt.hpp"
#include "bdd/tt_to_bdd.hpp"
#include "verify/reference_builders.hpp"
#include "verify/test_runner.hpp"

using namespace bmm;
using namespace bmm::verify;

namespace {
void report_metrics(const TestOutcome& outcome) {
    print_status_line(outcome, std::cout);
    INFO(outcome.detail);
    REQUIRE(outcome.status != TestStatus::Fail);
}

struct MetricGroup {
    std::vector<double> samples;
};

void print_grouped_metrics(const std::string& algo_name, const std::map<std::string, MetricGroup>& groups, const std::string& color_code) {
    std::cout << color_code << "\n==================================================================\n"
              << "  DETAILED ANALYSIS FOR: " << algo_name << "\n"
              << "==================================================================\n";
    
    for (const auto& [group_name, group_data] : groups) {
        auto samples = group_data.samples;
        if (samples.empty()) continue;

        std::sort(samples.begin(), samples.end());
        double total_us = std::accumulate(samples.begin(), samples.end(), 0.0);
        double avg_us = total_us / samples.size();
        double median_us = samples[samples.size() / 2];
        double max_us = samples.back();

        std::cout << "  -> Group [" << group_name << "] (" << samples.size() << " tests):\n"
                  << "     Avg: " << std::wsetw(7) << std::fixed << std::setprecision(2) << avg_us << " us | "
                  << "     Median: " << median_us << " us | "
                  << "     Max: " << max_us << " us\n";
    }
    std::cout << "==================================================================\033[0m\n\n";
}

std::string get_bdd_complexity_group(const Bdd& bdd) {
    // bdd.num_vars() — стандартный метод mockturtle/kitty для получения числа входов
    auto vars = bdd.num_vars(); 
    if (vars <= 4) return "Small (<=4 vars)";
    if (vars <= 8) return "Medium (5-8 vars)";
    return "Large (>8 vars)";
}
} // namespace

TEST_CASE("metrics_bdd_to_aig", "[metrics]") {
    std::map<std::string, MetricGroup> groups;

    auto timed_aig = [&groups](const Bdd& bdd) {
        std::string group_name = get_bdd_complexity_group(bdd);
        
        auto start = std::chrono::high_resolution_clock::now();
        auto result = bmm::bdd_to_aig(bdd);
        auto end = std::chrono::high_resolution_clock::now();
        
        std::chrono::duration<double, std::micro> duration = end - start;
        groups[group_name].samples.push_back(duration.count());
        return result;
    };

    report_metrics(run_translation_tests<Bdd, Aig>(
        "bdd_to_aig", std::function<Result<Aig>(const Bdd&)>(timed_aig),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));

    print_grouped_metrics("bdd_to_aig", groups, "\033[1;32m");
}

TEST_CASE("metrics_bdd_to_anf", "[metrics]") {
    std::map<std::string, MetricGroup> groups;

    auto timed_anf = [&groups](const Bdd& bdd) {
        std::string group_name = get_bdd_complexity_group(bdd);
        
        auto start = std::chrono::high_resolution_clock::now();
        auto result = bmm::bdd_to_anf(bdd);
        auto end = std::chrono::high_resolution_clock::now();
        
        std::chrono::duration<double, std::micro> duration = end - start;
        groups[group_name].samples.push_back(duration.count());
        return result;
    };

    report_metrics(run_translation_tests<Bdd, Anf>(
        "bdd_to_anf", std::function<Result<Anf>(const Bdd&)>(timed_anf),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));

    print_grouped_metrics("bdd_to_anf", groups, "\033[1;36m");
}
