// test_anf.cpp — 5 секций, по одной на каждую функцию трансляции в anf/.
// Запуск всех: `./test_anf`. Запуск одной: `./test_anf --section
// "anf_to_aig"` (см. README.md в корне репозитория).

#include <catch2/catch_test_macros.hpp>

#include <iostream>

#include "anf/anf_to_aig.hpp"
#include "anf/anf_to_bdd.hpp"
#include "anf/anf_to_thr.hpp"
#include "anf/anf_to_tt.hpp"
#include "anf/tt_to_anf.hpp"
#include "benchmarks/openmp_scaling.hpp"
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

// См. aig/test_aig.cpp — тот же независимый маленький генератор для
// TBB-бенчмарков, продублирован намеренно (не вынесен в общий заголовок
// verify/, чтобы не тянуть тестовую утилиту в публичный API verify/).
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
        if (!is_ok(input)) continue;
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

// Тот же харнесс, что run_tbb_bench выше, но через measure_scaling_omp
// (1 поток vs omp_get_max_threads(), benchmarks/openmp_scaling.hpp) — нужен
// ОТДЕЛЬНО от *_tbb_scaling именно для tt_to_anf/anf_to_tt: сам
// Мёбиус-трансформ распараллелен через OpenMP (см. doc-комментарий
// tt_to_anf.hpp), а не TBB, поэтому tbb::global_control в
// tt_to_anf_tbb_scaling НЕ управляет числом потоков, реально исполняющих
// transform, — та секция годится для будущей TBB-части (сборка Anf из
// коэффициентов), но не для самого transform. См. anf/README.md §9.5 за
// результатами и объяснением.
template <class Input>
void run_openmp_bench(const std::string& name,
                       const std::function<Result<Input>(const GroundTruthFunction&)>& build_input,
                       const std::function<void(const Input&)>& translate_once,
                       const std::function<bool(const Input&)>& is_implemented,
                       const std::vector<uint32_t>& sizes) {
    std::vector<bmm::benchmarks::ScalingPoint> points;
    for (uint32_t n : sizes) {
        const auto gt = make_bench_function(n);
        auto input = build_input(gt);
        if (!is_ok(input)) continue;
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
}  // namespace

TEST_CASE("tt_to_anf", "[anf]") {
    report(run_translation_tests<TruthTable, Anf>(
        "tt_to_anf", std::function<Result<Anf>(const TruthTable&)>(tt_to_anf),
        std::function<Result<TruthTable>(const GroundTruthFunction&)>(reference_truth_table)));
}

TEST_CASE("anf_to_aig", "[anf]") {
    report(run_translation_tests<Anf, Aig>(
        "anf_to_aig", std::function<Result<Aig>(const Anf&)>(anf_to_aig),
        std::function<Result<Anf>(const GroundTruthFunction&)>(reference_anf)));
}

TEST_CASE("anf_to_bdd", "[anf]") {
    report(run_translation_tests<Anf, Bdd>(
        "anf_to_bdd", std::function<Result<Bdd>(const Anf&)>(anf_to_bdd),
        std::function<Result<Anf>(const GroundTruthFunction&)>(reference_anf)));
}

TEST_CASE("anf_to_thr", "[anf]") {
    report(run_translation_tests<Anf, Thr>(
        "anf_to_thr", std::function<Result<Thr>(const Anf&)>(anf_to_thr),
        std::function<Result<Anf>(const GroundTruthFunction&)>(reference_anf)));
}

TEST_CASE("anf_to_tt", "[anf]") {
    report(run_translation_tests<Anf, TruthTable>(
        "anf_to_tt", std::function<Result<TruthTable>(const Anf&)>(anf_to_tt),
        std::function<Result<Anf>(const GroundTruthFunction&)>(reference_anf)));
}

// ---------------------------------------------------------------------------
// Обязательные TBB-бенчмарки (core/CONVENTIONS.md п.6, требование добавлено
// во второй итерации задания) — только для функций, которые ДЕЙСТВИТЕЛЬНО
// используют TBB по правилам приоритета из CONVENTIONS.md: tt_to_anf,
// anf_to_aig, anf_to_thr. anf_to_bdd сюда не входит — Bdd на выходе,
// правило 1 (Sylvan/Lace, никакого TBB). См. aig/test_aig.cpp за подробным
// объяснением идеи (сравнение одного и того же вызова под 1-поточным и
// полным tbb::global_control, а не отдельная "однопоточная копия" кода).
// ---------------------------------------------------------------------------

TEST_CASE("tt_to_anf_tbb_scaling", "[anf][benchmark]") {
    run_tbb_bench<TruthTable>(
        "tt_to_anf", std::function<Result<TruthTable>(const GroundTruthFunction&)>(reference_truth_table),
        std::function<void(const TruthTable&)>([](const TruthTable& tt) { (void)tt_to_anf(tt); }),
        std::function<bool(const TruthTable&)>([](const TruthTable& tt) {
            auto r = tt_to_anf(tt);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {8, 12, 16});
}

TEST_CASE("anf_to_aig_tbb_scaling", "[anf][benchmark]") {
    run_tbb_bench<Anf>(
        "anf_to_aig", std::function<Result<Anf>(const GroundTruthFunction&)>(reference_anf),
        std::function<void(const Anf&)>([](const Anf& anf) { (void)anf_to_aig(anf); }),
        std::function<bool(const Anf&)>([](const Anf& anf) {
            auto r = anf_to_aig(anf);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {8, 12, 16});
}

TEST_CASE("anf_to_thr_tbb_scaling", "[anf][benchmark]") {
    run_tbb_bench<Anf>(
        "anf_to_thr", std::function<Result<Anf>(const GroundTruthFunction&)>(reference_anf),
        std::function<void(const Anf&)>([](const Anf& anf) { (void)anf_to_thr(anf); }),
        std::function<bool(const Anf&)>([](const Anf& anf) {
            auto r = anf_to_thr(anf);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {8, 12, 16});
}

// ---------------------------------------------------------------------------
// OpenMP-бенчмарки Мёбиус-трансформа (см. run_openmp_bench выше за тем, чем
// это отличается от *_tbb_scaling). Размеры намеренно охватывают порог
// kParallelThreshold=2^18 в anf/tt_to_anf.cpp/anf/anf_to_tt.cpp (n>=18 ->
// параллельная ветка, n<18 -> последовательная) — n=12/16 должны показать
// speedup~1.0 (параллельный код вообще не исполняется), n=18/20/22/24 —
// реальный эффект OpenMP, если он есть. n=24 = kMaxTruthTableVars, дальше
// TruthTable не строится в принципе.
// ---------------------------------------------------------------------------

TEST_CASE("tt_to_anf_openmp_scaling", "[anf][benchmark]") {
    run_openmp_bench<TruthTable>(
        "tt_to_anf", std::function<Result<TruthTable>(const GroundTruthFunction&)>(reference_truth_table),
        std::function<void(const TruthTable&)>([](const TruthTable& tt) { (void)tt_to_anf(tt); }),
        std::function<bool(const TruthTable&)>([](const TruthTable& tt) {
            auto r = tt_to_anf(tt);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {12, 16, 18, 20, 22, 24});
}

TEST_CASE("anf_to_tt_openmp_scaling", "[anf][benchmark]") {
    run_openmp_bench<Anf>(
        "anf_to_tt", std::function<Result<Anf>(const GroundTruthFunction&)>(reference_anf),
        std::function<void(const Anf&)>([](const Anf& anf) { (void)anf_to_tt(anf); }),
        std::function<bool(const Anf&)>([](const Anf& anf) {
            auto r = anf_to_tt(anf);
            return is_ok(r) || error(r).code != ErrorCode::NotImplemented;
        }),
        {12, 16, 18, 20, 22, 24});
}
