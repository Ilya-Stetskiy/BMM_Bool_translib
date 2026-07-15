// test_thr.cpp — 5 секций, по одной на каждую функцию трансляции в thr/.
// Запуск всех: `./test_thr`. Запуск одной: `./test_thr --section
// "thr_to_aig"` (см. README.md в корне репозитория).
//
// Источник для *_to_thr функций (tt_to_thr) — обычный
// verify::reference_truth_table + общий growing_test_functions() набор:
// tt_to_thr сам обязан отличить пороговые входы от непороговых (см.
// thr/tt_to_thr.hpp), поэтому тестируется на произвольных функциях.
//
// Источник для thr_to_* функций — НЕ общий набор (Thr не умеет представлять
// произвольную булеву функцию): используется
// verify::growing_threshold_test_functions(), обёрнутый в GroundTruthFunction
// через verify::ground_truth_from_thr(), с build_source, находящим исходный
// Thr по имени тестового случая (см. threshold_test_set() ниже).

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "thr/thr_to_aig.hpp"
#include "thr/thr_to_anf.hpp"
#include "thr/thr_to_bdd.hpp"
#include "thr/thr_to_tt.hpp"
#include "thr/tt_to_thr.hpp"
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

struct ThresholdTestSet {
    std::vector<GroundTruthFunction> ground_truths;
    std::function<Result<Thr>(const GroundTruthFunction&)> build_source;
};

ThresholdTestSet make_threshold_test_set(uint32_t max_n) {
    auto thresholds = std::make_shared<std::vector<Thr>>(growing_threshold_test_functions(max_n));

    ThresholdTestSet set;
    set.ground_truths.reserve(thresholds->size());
    for (size_t i = 0; i < thresholds->size(); ++i) {
        set.ground_truths.push_back(
            ground_truth_from_thr((*thresholds)[i], "thr#" + std::to_string(i)));
    }
    set.build_source = [thresholds](const GroundTruthFunction& gt) -> Result<Thr> {
        for (size_t i = 0; i < thresholds->size(); ++i) {
            if (gt.name == "thr#" + std::to_string(i)) return ok<Thr>((*thresholds)[i]);
        }
        return fail<Thr>(ErrorCode::InvalidArgument, "make_threshold_test_set: неизвестное имя " + gt.name);
    };
    return set;
}

}  // namespace

TEST_CASE("tt_to_thr", "[thr]") {
    report(run_translation_tests<TruthTable, Thr>(
        "tt_to_thr", std::function<Result<Thr>(const TruthTable&)>(tt_to_thr),
        std::function<Result<TruthTable>(const GroundTruthFunction&)>(reference_truth_table)));
}

TEST_CASE("thr_to_aig", "[thr]") {
    auto set = make_threshold_test_set(kMaxGroundTruthVars);
    report(run_translation_tests<Thr, Aig>("thr_to_aig",
                                            std::function<Result<Aig>(const Thr&)>(thr_to_aig),
                                            set.build_source, set.ground_truths));
}

TEST_CASE("thr_to_bdd", "[thr]") {
    auto set = make_threshold_test_set(kMaxGroundTruthVars);
    report(run_translation_tests<Thr, Bdd>("thr_to_bdd",
                                            std::function<Result<Bdd>(const Thr&)>(thr_to_bdd),
                                            set.build_source, set.ground_truths));
}

TEST_CASE("thr_to_anf", "[thr]") {
    auto set = make_threshold_test_set(kMaxGroundTruthVars);
    report(run_translation_tests<Thr, Anf>("thr_to_anf",
                                            std::function<Result<Anf>(const Thr&)>(thr_to_anf),
                                            set.build_source, set.ground_truths));
}

TEST_CASE("thr_to_tt", "[thr]") {
    auto set = make_threshold_test_set(kMaxGroundTruthVars);
    report(run_translation_tests<Thr, TruthTable>(
        "thr_to_tt", std::function<Result<TruthTable>(const Thr&)>(thr_to_tt), set.build_source,
        set.ground_truths));
}
