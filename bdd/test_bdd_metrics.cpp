// test_bdd.cpp — 5 секций, по одной на каждую функцию трансляции в bdd/.
// Запуск всех: `./test_bdd`. Запуск одной: `./test_bdd --section
// "bdd_to_aig"` (см. README.md в корне репозитория).

#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm> // Для std::sort
#include <numeric>   // Для std::accumulate
#include <iomanip>   // Для красивого форматирования чисел

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
void report(const TestOutcome& outcome) {
    print_status_line(outcome, std::cout);
    INFO(outcome.detail);
    REQUIRE(outcome.status != TestStatus::Fail);
}

// Вспомогательная функция для подсчета расширенной статистики
void print_extended_metrics(const std::string& name, std::vector<double>& samples, const std::string& color_code) {
    if (samples.empty()) return;

    // Сортируем для поиска медианы, процентилей, min и max
    std::sort(samples.begin(), samples.end());

    double total_us = std::accumulate(samples.begin(), samples.end(), 0.0);
    double avg_us = total_us / samples.size();
    double min_us = samples.front();
    double max_us = samples.back();
    
    // Вычисляем медиану и p95
    double median_us = samples[samples.size() / 2];
    size_t p95_index = static_cast<size_t>(samples.size() * 0.95);
    if (p95_index >= samples.size()) p95_index = samples.size() - 1;
    double p95_us = samples[p95_index];

    // Выводим структурированный блок данных
    std::cout << color_code << "\n==================================================\n"
              << "  METRICS FOR: " << name << "\n"
              << "==================================================\n"
              << "  Total Calls : " << samples.size() << "\n"
              << "  Total Time  : " << std::fixed << std::setprecision(3) << (total_us / 1000.0) << " ms\n"
              << "  Average     : " << avg_us << " us\n"
              << "  Median (p50): " << median_us << " us\n"
              << "  95th%  (p95): " << p95_us << " us\n"
              << "  Minimum     : " << min_us << " us\n"
              << "  Maximum     : " << max_us << " us\n"
              << "==================================================\033[0m\n\n";
}
}  // namespace

TEST_CASE("tt_to_bdd", "[bdd]") {
    report(run_translation_tests<TruthTable, Bdd>(
        "tt_to_bdd", std::function<Result<Bdd>(const TruthTable&)>(bmm::tt_to_bdd),
        std::function<Result<TruthTable>(const GroundTruthFunction&)>(reference_truth_table)));
}

TEST_CASE("bdd_to_aig", "[bdd]") {
    std::vector<double> samples;

    auto timed_bdd_to_aig = [&samples](const Bdd& bdd) {
        auto start = std::chrono::high_resolution_clock::now();
        
        auto result = bmm::bdd_to_aig(bdd);
        
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::micro> duration = end - start;
        
        samples.push_back(duration.count());
        return result;
    };

    report(run_translation_tests<Bdd, Aig>(
        "bdd_to_aig", std::function<Result<Aig>(const Bdd&)>(timed_bdd_to_aig),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));

    // Зеленый цвет для AIG
    print_extended_metrics("bdd_to_aig", samples, "\033[1;32m");
}

TEST_CASE("bdd_to_anf", "[bdd]") {
    std::vector<double> samples;

    auto timed_bdd_to_anf = [&samples](const Bdd& bdd) {
        auto start = std::chrono::high_resolution_clock::now();
        
        auto result = bmm::bdd_to_anf(bdd);
        
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::micro> duration = end - start;
        
        samples.push_back(duration.count());
        return result;
    };

    report(run_translation_tests<Bdd, Anf>(
        "bdd_to_anf", std::function<Result<Anf>(const Bdd&)>(timed_bdd_to_anf),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));

    // Бирюзовый цвет для ANF
    print_extended_metrics("bdd_to_anf", samples, "\033[1;36m");
}

TEST_CASE("bdd_to_thr", "[bdd]") {
    report(run_translation_tests<Bdd, Thr>(
        "bdd_to_thr", std::function<Result<Thr>(const Bdd&)>(bmm::bdd_to_thr),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));
}

TEST_CASE("bdd_to_tt", "[bdd]") {
    report(run_translation_tests<Bdd, TruthTable>(
        "bdd_to_tt", std::function<Result<TruthTable>(const Bdd&)>(bmm::bdd_to_tt),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));
}
