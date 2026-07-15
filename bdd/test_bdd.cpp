// test_bdd.cpp — 5 секций, по одной на каждую функцию трансляции в bdd/.
// Запуск всех: `./test_bdd`. Запуск одной: `./test_bdd --section
// "bdd_to_aig"` (см. README.md в корне репозитория).

#include <catch2/catch_test_macros.hpp>

#include <iostream>

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
}  // namespace

TEST_CASE("tt_to_bdd", "[bdd]") {
    report(run_translation_tests<TruthTable, Bdd>(
        "tt_to_bdd", std::function<Result<Bdd>(const TruthTable&)>(tt_to_bdd),
        std::function<Result<TruthTable>(const GroundTruthFunction&)>(reference_truth_table)));
}

TEST_CASE("bdd_to_aig", "[bdd]") {
    report(run_translation_tests<Bdd, Aig>(
        "bdd_to_aig", std::function<Result<Aig>(const Bdd&)>(bdd_to_aig),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));
}

TEST_CASE("bdd_to_anf", "[bdd]") {
    report(run_translation_tests<Bdd, Anf>(
        "bdd_to_anf", std::function<Result<Anf>(const Bdd&)>(bdd_to_anf),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));
}

TEST_CASE("bdd_to_thr", "[bdd]") {
    // bdd_to_thr — "тёмная ночь" (см. bdd/bdd_to_thr.hpp): скорее всего
    // останется SKIP дольше остальных, это ожидаемо.
    report(run_translation_tests<Bdd, Thr>(
        "bdd_to_thr", std::function<Result<Thr>(const Bdd&)>(bdd_to_thr),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));
}

TEST_CASE("bdd_to_tt", "[bdd]") {
    report(run_translation_tests<Bdd, TruthTable>(
        "bdd_to_tt", std::function<Result<TruthTable>(const Bdd&)>(bdd_to_tt),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));
}
