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
    // ИСПРАВЛЕНО (см. bdd/README.md §5.4, пункт 5 дорожной карты): раньше
    // здесь использовался общий growing_test_functions() (дефолтный
    // 4-аргументный overload run_translation_tests) — набор ПРОИЗВОЛЬНЫХ
    // функций (XOR и т.п.), почти ни одна из которых не является пороговой,
    // так что тест по факту проверял в основном "правильно ли bdd_to_thr
    // ОТКАЗЫВАЕТСЯ" (ErrorCode::Unsupported), а не распознавание реальных
    // пороговых функций через CHOW_DATABASE. build_source остаётся
    // reference_bdd (Bdd умеет представить любую функцию, в отличие от Thr,
    // поэтому отдельный build_source не нужен — не то же самое, что паттерн
    // thr_to_*() в thr/test_thr.cpp) — заменены только сами тестовые
    // функции на growing_threshold_test_functions(), обёрнутые в
    // GroundTruthFunction тем же ground_truth_from_thr(), что и в
    // thr/test_thr.cpp/core/test_core.cpp.
    // Ограничено n<=6 — реальный поддерживаемый диапазон bdd_to_thr сейчас
    // (K>6 -> ErrorCode::NotImplemented по построению, см.
    // bdd_to_thr.cpp/bdd/bdd_to_thr.hpp): любой тестовый случай с n>6
    // немедленно вызвал бы общий SKIP всего теста (run_translation_tests
    // останавливается на первом NotImplemented), что скрыло бы реальный
    // результат работы CHOW_DATABASE на n<=6 за посторонним, уже известным
    // и отдельно задокументированным ограничением (roadmap п.3 в
    // bdd/README.md §5.4 — поднять до n<=8 не входит в объём этой задачи).
    auto thresholds = growing_threshold_test_functions(6);
    std::vector<GroundTruthFunction> threshold_ground_truths;
    threshold_ground_truths.reserve(thresholds.size());
    for (size_t i = 0; i < thresholds.size(); ++i) {
        threshold_ground_truths.push_back(
            ground_truth_from_thr(thresholds[i], "thr#" + std::to_string(i)));
    }

    report(run_translation_tests<Bdd, Thr>(
        "bdd_to_thr", std::function<Result<Thr>(const Bdd&)>(bdd_to_thr),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd),
        threshold_ground_truths));
}

TEST_CASE("bdd_to_tt", "[bdd]") {
    report(run_translation_tests<Bdd, TruthTable>(
        "bdd_to_tt", std::function<Result<TruthTable>(const Bdd&)>(bdd_to_tt),
        std::function<Result<Bdd>(const GroundTruthFunction&)>(reference_bdd)));
}
