// core/test_core.cpp — покрытие core/common.hpp, которое раньше нигде не
// проверялось: каждый Representation (Aig/Bdd/Anf/Thr) реализует .to_tt()
// напрямую в common.hpp (это НЕ стаб и НЕ входит в 20 функций трансляции из
// aig/,bdd/,anf/,thr/) — но ни test_runner.hpp, ни один из
// test_<format>.cpp его не вызывают: run_translation_tests() тестирует
// именно функции трансляции, а не сам .to_tt(). Код .to_tt() оказался
// корректным при ручной проверке, но без этого файла регрессия здесь
// прошла бы незамеченной. Проверка прямая: для каждого представления
// перебираем все точки, сравниваем X.to_tt()-производную TruthTable с
// ground truth — независимо от reference_builders'ов, которые используются
// как ЕДИНСТВЕННЫЙ способ материализовать X (то есть транзитивно доверяем
// им так же, как test_<format>.cpp доверяет для build_source).

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "verify/reference_builders.hpp"
#include "verify/test_runner.hpp"

using namespace bmm;
using namespace bmm::verify;

namespace {

// Прямая проверка X::to_tt() против ground truth (не через
// run_translation_tests — тот тестирует функции трансляции, не .to_tt()).
template <Representation Repr>
void check_to_tt(const std::string& label, const Repr& r, const GroundTruthFunction& gt) {
    auto tt_result = r.to_tt();
    INFO(label + " " + gt.name + ": to_tt() вернул ошибку");
    REQUIRE(is_ok(tt_result));
    const TruthTable& tt = value(tt_result);
    for (uint64_t idx = 0; idx < (uint64_t{1} << gt.n_vars); ++idx) {
        Assignment a(gt.n_vars);
        for (uint32_t b = 0; b < gt.n_vars; ++b) a[b] = (idx >> b) & 1u;
        INFO(label + " " + gt.name + " разошлась в точке #" + std::to_string(idx));
        REQUIRE(tt.evaluate(a) == gt.evaluate(idx));
    }
}

}  // namespace

TEST_CASE("Aig::to_tt", "[core]") {
    // reference_aig ограничен kMaxReferenceAigVars (экспоненциальное
    // MUX-дерево) — за пределом build_source возвращает TooManyVariables,
    // такие случаи пропускаем, не считаем FAIL (та же логика, что и в
    // run_translation_tests для build_source).
    for (const auto& gt : growing_test_functions(kMaxGroundTruthVars)) {
        auto built = reference_aig(gt);
        if (!is_ok(built)) continue;
        check_to_tt("Aig", value(built), gt);
    }
}

TEST_CASE("Bdd::to_tt", "[core]") {
    for (const auto& gt : growing_test_functions(kMaxGroundTruthVars)) {
        check_to_tt("Bdd", value(reference_bdd(gt)), gt);
    }
}

TEST_CASE("Anf::to_tt", "[core]") {
    for (const auto& gt : growing_test_functions(kMaxGroundTruthVars)) {
        check_to_tt("Anf", value(reference_anf(gt)), gt);
    }
}

TEST_CASE("Thr::to_tt", "[core]") {
    for (const auto& thr : growing_threshold_test_functions(kMaxGroundTruthVars)) {
        const auto gt = ground_truth_from_thr(thr, "thr_n" + std::to_string(thr.n_vars()));
        check_to_tt("Thr", thr, gt);
    }
}