// test_aig.cpp — 5 секций, по одной на каждую функцию трансляции в aig/.
// Запуск всех: `./test_aig`. Запуск одной: `./test_aig --section
// "aig_to_bdd"` (см. README.md в корне репозитория).
//
// До того как студенты что-либо реализуют, все 5 секций проходят Catch2 с
// SKIP-статусом (см. verify/test_runner.hpp) — тесты компилируются и
// зелёные сразу после генерации репозитория.

#include <catch2/catch_test_macros.hpp>

#include <iostream>

#include "aig/aig_to_anf.hpp"
#include "aig/aig_to_bdd.hpp"
#include "aig/aig_to_thr.hpp"
#include "aig/aig_to_tt.hpp"
#include "aig/tt_to_aig.hpp"
#include "benchmarks/tbb_scaling.hpp"
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

// Небольшая "структурная" (не константа, не чистая проекция) тестовая
// функция для бенчмарка — majority-подобная, чтобы реальная работа
// TBB-алгоритма на ней была нетривиальной. Не переиспользует внутренности
// verify/ground_truth/ground_truth.cpp (те — деталь реализации, не
// публичный API) — независимый маленький генератор прямо здесь.
GroundTruthFunction make_bench_function(uint32_t n) {
    GroundTruthFunction gt;
    gt.name = "bench_n" + std::to_string(n);
    gt.n_vars = n;
    const uint64_t rows = uint64_t{1} << n;
    gt.table.resize(rows);
    for (uint64_t idx = 0; idx < rows; ++idx) {
        gt.table[idx] = static_cast<uint32_t>(__builtin_popcountll(idx)) > n / 2;
    }
    return gt;
}
}  // namespace

TEST_CASE("tt_to_aig", "[aig]") {
    report(run_translation_tests<TruthTable, Aig>(
        "tt_to_aig", std::function<Result<Aig>(const TruthTable&)>(tt_to_aig),
        std::function<Result<TruthTable>(const GroundTruthFunction&)>(reference_truth_table)));
}

TEST_CASE("aig_to_bdd", "[aig]") {
    report(run_translation_tests<Aig, Bdd>(
        "aig_to_bdd", std::function<Result<Bdd>(const Aig&)>(aig_to_bdd),
        std::function<Result<Aig>(const GroundTruthFunction&)>(reference_aig)));
}

TEST_CASE("aig_to_anf", "[aig]") {
    report(run_translation_tests<Aig, Anf>(
        "aig_to_anf", std::function<Result<Anf>(const Aig&)>(aig_to_anf),
        std::function<Result<Aig>(const GroundTruthFunction&)>(reference_aig)));
}

TEST_CASE("aig_to_thr", "[aig]") {
    report(run_translation_tests<Aig, Thr>(
        "aig_to_thr", std::function<Result<Thr>(const Aig&)>(aig_to_thr),
        std::function<Result<Aig>(const GroundTruthFunction&)>(reference_aig)));
}

TEST_CASE("aig_to_tt", "[aig]") {
    report(run_translation_tests<Aig, TruthTable>(
        "aig_to_tt", std::function<Result<TruthTable>(const Aig&)>(aig_to_tt),
        std::function<Result<Aig>(const GroundTruthFunction&)>(reference_aig)));
}

// ---------------------------------------------------------------------------
// Обязательные TBB-бенчмарки (core/CONVENTIONS.md п.6, требование добавлено
// во второй итерации задания) — только для функций, которые ДЕЙСТВИТЕЛЬНО
// используют TBB по правилам приоритета из CONVENTIONS.md (не все 5 функций
// в этой папке: aig_to_bdd — Sylvan/Lace, aig_to_tt — OpenMP). См.
// benchmarks/tbb_scaling.hpp за объяснением, почему сравниваем один и тот
// же вызов под 1-поточным и полным tbb::global_control, а не пишем
// отдельную "однопоточную копию" алгоритма.
//
// Пока функция не реализована (NotImplemented) — секция печатает SKIP и
// не тратит время на прогрев/замеры; это ожидаемо сразу после генерации
// репозитория, не ошибка.
// ---------------------------------------------------------------------------

namespace {
// build_input: строит вход ОДИН РАЗ на размер (вне замера — конструирование
// эталонного входа не то, что мы бенчмаркаем). translate_once: сам вызов
// функции трансляции — вот это и замеряется. Возвращает false, если
// translate() вернул NotImplemented (вся функция ещё не написана — SKIP).
template <class Input>
void run_tbb_bench(const std::string& name,
                    const std::function<Result<Input>(const GroundTruthFunction&)>& build_input,
                    const std::function<void(const Input&)>& translate_once,
                    const std::function<bool(const Input&)>& is_implemented,
                    const std::vector<uint32_t>& sizes) {
    std::vector<bmm::benchmarks::ScalingPoint> points;
    for (uint32_t n : sizes) {
        const auto gt = make_bench_function(n);
        auto input = build_input(gt);
        if (!is_ok(input)) continue;  // этот размер не построить (напр. n > kMaxReferenceAigVars) — пропуск размера
        if (!is_implemented(value(input))) {
            std::cout << "BMM_BENCH_SUMMARY " << name << " SKIP (не реализовано)\n";
            return;
        }
        auto point = bmm::benchmarks::measure_scaling(
            "n=" + std::to_string(n), [&] { translate_once(value(input)); });
        bmm::benchmarks::print_bench_line(name, point, std::cout);
        points.push_back(point);
    }
    bmm::benchmarks::print_bench_summary(name, points, std::cout);
}
}  // namespace

TEST_CASE("tt_to_aig_tbb_scaling", "[aig][benchmark]") {
    run_tbb_bench<TruthTable>(
        "tt_to_aig", std::function<Result<TruthTable>(const GroundTruthFunction&)>(reference_truth_table),
        std::function<void(const TruthTable&)>([](const TruthTable& tt) { (void)tt_to_aig(tt); }),
        std::function<bool(const TruthTable&)>([](const TruthTable& tt) {
            auto r = tt_to_aig(tt);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {8, 12, 16});
}

TEST_CASE("aig_to_anf_tbb_scaling", "[aig][benchmark]") {
    run_tbb_bench<Aig>(
        "aig_to_anf", std::function<Result<Aig>(const GroundTruthFunction&)>(reference_aig),
        std::function<void(const Aig&)>([](const Aig& aig) { (void)aig_to_anf(aig); }),
        std::function<bool(const Aig&)>([](const Aig& aig) {
            auto r = aig_to_anf(aig);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {8, 12, kMaxReferenceAigVars});
}

TEST_CASE("aig_to_thr_tbb_scaling", "[aig][benchmark]") {
    run_tbb_bench<Aig>(
        "aig_to_thr", std::function<Result<Aig>(const GroundTruthFunction&)>(reference_aig),
        std::function<void(const Aig&)>([](const Aig& aig) { (void)aig_to_thr(aig); }),
        std::function<bool(const Aig&)>([](const Aig& aig) {
            auto r = aig_to_thr(aig);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {8, 12, kMaxReferenceAigVars});
}
