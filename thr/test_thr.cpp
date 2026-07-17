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
//
// OpenMP-бенчмарки (секции "*_openmp_scaling") — см. core/CONVENTIONS.md
// п.6: thr/* (кроме thr_to_bdd — тот идёт по правилу "Bdd — только
// Sylvan/Lace" с более высоким приоритетом) обязаны использовать OpenMP.
// Харнесс замера — benchmarks/openmp_scaling.hpp, тот же формат вывода
// (BMM_BENCH/BMM_BENCH_SUMMARY), что и у TBB-бенчмарков в
// aig/test_aig.cpp и anf/test_anf.cpp (общий benchmarks/scaling.hpp).

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "benchmarks/openmp_scaling.hpp"
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

// ---------------------------------------------------------------------------
// Обязательные OpenMP-бенчмарки (core/CONVENTIONS.md п.6) — для thr/* КРОМЕ
// thr_to_bdd (тот — только Sylvan/Lace, никакого OpenMP, см. правило 1 в
// приоритете параллелизма). Тот же принцип, что TBB-бенчмарки в
// aig/test_aig.cpp/anf/test_anf.cpp: замеряем ОДИН И ТОТ ЖЕ вызов под 1
// потоком и под дефолтным числом потоков (benchmarks/openmp_scaling.hpp),
// а не пишем отдельную "однопоточную копию" алгоритма.
//
// Пока функция не реализована (NotImplemented) — секция печатает SKIP и не
// тратит время на прогрев/замеры; это ожидаемо сразу после генерации
// репозитория, не ошибка.
// ---------------------------------------------------------------------------

namespace {

// Пороговая функция-заглушка для бенчмарка: все веса 1, порог — "чуть больше
// половины" (majority-подобная), чтобы реальная работа thr_to_*-алгоритма на
// ней была нетривиальной. Независима от verify::growing_threshold_test_functions
// (та — для корректности, с случайными весами и детерминированным seed; здесь
// нужен просто предсказуемый вход растущего размера для замера времени).
Thr make_bench_threshold(uint32_t n) {
    std::vector<int64_t> weights(n, 1);
    return Thr(std::move(weights), static_cast<int64_t>((n + 1) / 2));
}

// См. aig/test_aig.cpp — тот же независимый маленький генератор для
// бенчмарков truth-table-based входа (tt_to_thr), продублирован намеренно
// (не вынесен в общий заголовок verify/, чтобы не тянуть тестовую утилиту в
// публичный API verify/).
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

// Бенчмарк для tt_to_thr (TruthTable на входе).
void run_openmp_bench_from_tt(const std::string& name,
                               const std::function<void(const TruthTable&)>& translate_once,
                               const std::function<bool(const TruthTable&)>& is_implemented,
                               const std::vector<uint32_t>& sizes) {
    std::vector<bmm::benchmarks::ScalingPoint> points;
    for (uint32_t n : sizes) {
        const auto gt = make_bench_function(n);
        auto input = reference_truth_table(gt);
        if (!is_ok(input)) continue;  // этот размер не построить — пропуск размера
        if (!is_implemented(value(input))) {
            std::cout << "BMM_BENCH_SUMMARY " << name << " SKIP (не реализовано)\n";
            return;
        }
        auto point = bmm::benchmarks::measure_scaling_omp(
            "n=" + std::to_string(n), [&] { translate_once(value(input)); });
        bmm::benchmarks::print_bench_line(name, point, std::cout);
        points.push_back(point);
    }
    bmm::benchmarks::print_bench_summary(name, points, std::cout);
}

// Бенчмарк для thr_to_aig/thr_to_anf/thr_to_tt (Thr на входе).
void run_openmp_bench_from_thr(const std::string& name,
                                const std::function<void(const Thr&)>& translate_once,
                                const std::function<bool(const Thr&)>& is_implemented,
                                const std::vector<uint32_t>& sizes) {
    std::vector<bmm::benchmarks::ScalingPoint> points;
    for (uint32_t n : sizes) {
        const Thr input = make_bench_threshold(n);
        if (!is_implemented(input)) {
            std::cout << "BMM_BENCH_SUMMARY " << name << " SKIP (не реализовано)\n";
            return;
        }
        auto point = bmm::benchmarks::measure_scaling_omp(
            "n=" + std::to_string(n), [&] { translate_once(input); });
        bmm::benchmarks::print_bench_line(name, point, std::cout);
        points.push_back(point);
    }
    bmm::benchmarks::print_bench_summary(name, points, std::cout);
}

}  // namespace

TEST_CASE("tt_to_thr_openmp_scaling", "[thr][benchmark]") {
    run_openmp_bench_from_tt(
        "tt_to_thr",
        std::function<void(const TruthTable&)>([](const TruthTable& tt) { (void)tt_to_thr(tt); }),
        std::function<bool(const TruthTable&)>([](const TruthTable& tt) {
            auto r = tt_to_thr(tt);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {8, 12, 16});
}

TEST_CASE("thr_to_aig_openmp_scaling", "[thr][benchmark]") {
    run_openmp_bench_from_thr(
        "thr_to_aig",
        std::function<void(const Thr&)>([](const Thr& thr) { (void)thr_to_aig(thr); }),
        std::function<bool(const Thr&)>([](const Thr& thr) {
            auto r = thr_to_aig(thr);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {8, 12, 16});
}

TEST_CASE("thr_to_anf_openmp_scaling", "[thr][benchmark]") {
    run_openmp_bench_from_thr(
        "thr_to_anf",
        std::function<void(const Thr&)>([](const Thr& thr) { (void)thr_to_anf(thr); }),
        std::function<bool(const Thr&)>([](const Thr& thr) {
            auto r = thr_to_anf(thr);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {8, 12, 16});
}

TEST_CASE("thr_to_tt_openmp_scaling", "[thr][benchmark]") {
    run_openmp_bench_from_thr(
        "thr_to_tt",
        std::function<void(const Thr&)>([](const Thr& thr) { (void)thr_to_tt(thr); }),
        std::function<bool(const Thr&)>([](const Thr& thr) {
            auto r = thr_to_tt(thr);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {8, 12, 16});
}
