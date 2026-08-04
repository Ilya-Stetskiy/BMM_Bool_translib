// verify/large_n_tests.cpp — независимая проверка корректности на n в
// диапазоне, открытом поднятием kMaxTruthTableVars 24 -> 32 (core/
// common.hpp). Ни test_aig/test_bdd/test_anf/test_thr (ограничены
// kMaxGroundTruthVars=12, verify/ground_truth/ground_truth.hpp), ни
// chain_tests/full_matrix_tests (growing_test_functions/growing_threshold_
// test_functions — та же граница или меньше), ни anf/bench_real_corpus.cpp
// (чистый замер времени, НЕ сверяет результат ни с чем) не проверяют этот
// диапазон вообще — этот файл специально закрывает именно его.
//
// Эталон — НЕ verify::reference_*/growing_test_functions (те сами
// ограничены kMaxGroundTruthVars и не годятся при n>12), а независимая
// математическая формула, вычисляемая прямо здесь, без единого вызова
// кода из aig/anf/bdd/thr — тот же принцип "не проверять код сам на себе",
// что и во всём остальном verify/ (core/CONVENTIONS.md п.7).
//
// Функции: чётность (XOR всех переменных, ANF = сумма n одночленов x_i) —
// простая, но НЕТРИВИАЛЬНАЯ структура для теста Мёбиус-преобразования
// (не константа, не один моном, значение реально зависит от паритета
// каждой точки) с точной независимой формулой: f(x) = popcount(x) & 1.
//
// ПОСТОЯННЫЙ таргет (не удаляется после того как числа записаны, как
// test_chains.cpp/full_matrix_tests.cpp) — актуален всегда, пока
// kMaxTruthTableVars остаётся в текущих границах.

#include <bmm/anf/anf_to_tt.hpp>
#include <bmm/anf/tt_to_anf.hpp>
#include <bmm/core/anf_repr.hpp>
#include <bmm/core/common.hpp>

#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

using namespace bmm;

namespace {

int g_total = 0;
int g_failed = 0;

void check(bool ok, const std::string& text) {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text.c_str());
    std::fflush(stdout);
}

bool parity_bit(uint64_t idx) {
    return (__builtin_popcountll(idx) & 1) != 0;
}

// ANF чётности — независимая от tt_to_anf конструкция: ровно n одночленов
// {x_0}, {x_1}, ..., {x_{n-1}}, ни одного лишнего. Строим напрямую через
// AnfFallback/BoolePolynomial в обход какого-либо кода 20 функций.
Anf build_parity_anf(uint32_t n) {
#if BMM_HAVE_BRIAL
    polybori::BoolePolyRing ring(n);
    polybori::BoolePolynomial poly(ring);
    for (uint32_t i = 0; i < n; ++i) {
        poly += ring.variable(i);
    }
    return Anf(std::move(poly), n);
#else
    AnfFallback poly;
    for (uint32_t i = 0; i < n; ++i) {
        poly.add_monomial({i});
    }
    return Anf(std::move(poly), n);
#endif
}

// anf_to_tt(parity) должен дать ровно f(idx) = parity_bit(idx) на каждой
// точке. Для n<=kExhaustiveLimit — перебор всех 2^n точек; для больших n —
// случайная выборка (тот же приём, что verify::chains::reprs_equivalent_
// sampled — 2^32 точек полным перебором физически не пройти за разумное
// время теста).
void check_anf_to_tt(uint32_t n, uint64_t exhaustive_limit) {
    Anf anf = build_parity_anf(n);

    auto result = anf_to_tt(anf);
    if (!is_ok(result)) {
        check(false, "anf_to_tt(parity_" + std::to_string(n) +
                          "): вызов не удался — " + error(result).message);
        return;
    }
    const TruthTable& tt = value(result);

    const uint64_t rows = uint64_t{1} << n;
    bool all_ok = true;
    uint64_t first_bad = 0;

    if (rows <= exhaustive_limit) {
        for (uint64_t idx = 0; idx < rows; ++idx) {
            if (kitty::get_bit(tt.raw(), idx) != parity_bit(idx)) {
                all_ok = false;
                first_bad = idx;
                break;
            }
        }
        check(all_ok, "anf_to_tt(parity_" + std::to_string(n) + "): полный перебор 2^" +
                           std::to_string(n) + " точек" +
                           (all_ok ? "" : " — первое расхождение на idx=" + std::to_string(first_bad)));
    } else {
        std::mt19937_64 rng(1000 + n);
        std::uniform_int_distribution<uint64_t> dist(0, rows - 1);
        constexpr int kSamples = 2'000'000;
        for (int s = 0; s < kSamples; ++s) {
            uint64_t idx = dist(rng);
            if (kitty::get_bit(tt.raw(), idx) != parity_bit(idx)) {
                all_ok = false;
                first_bad = idx;
                break;
            }
        }
        check(all_ok, "anf_to_tt(parity_" + std::to_string(n) + "): выборка " +
                           std::to_string(kSamples) + " случайных точек из 2^" + std::to_string(n) +
                           (all_ok ? "" : " — расхождение на idx=" + std::to_string(first_bad)));
    }
}

// tt_to_anf(тот же паритет, построенный НАПРЯМУЮ по формуле, а не через
// anf_to_tt выше — независимая проверка обратного преобразования) должен
// дать ANF ровно с n одночленами {x_0}..{x_{n-1}}, без единого лишнего.
void check_tt_to_anf(uint32_t n, uint64_t exhaustive_limit) {
    const uint64_t rows = uint64_t{1} << n;
    if (rows > exhaustive_limit) {
        // Заполнение TruthTable по формуле само по себе O(2^n) — тот же
        // экономный путь: на n=32 это отдельная, самостоятельная причина
        // не делать это полным перебором в тесте, который должен
        // укладываться в разумное время. Пропускаем tt_to_anf на этих n —
        // anf_to_tt (выше) и без того выборочно покрывает n=32 c той же
        // формулой в другую сторону.
        return;
    }

    TruthTable tt(n);
    for (uint64_t idx = 0; idx < rows; ++idx) {
        if (parity_bit(idx)) kitty::set_bit(tt.raw(), idx);
    }

    auto result = tt_to_anf(tt);
    if (!is_ok(result)) {
        check(false, "tt_to_anf(parity_" + std::to_string(n) +
                          "): вызов не удался — " + error(result).message);
        return;
    }
    const Anf& anf = value(result);

    // Независимая проверка структуры ANF: ровно n одночленов, каждый —
    // единственная переменная, без повторов и без пустого/составного монома.
#if BMM_HAVE_BRIAL
    size_t term_count = 0;
    bool structure_ok = true;
    for (auto it = anf.raw().begin(); it != anf.raw().end(); ++it) {
        ++term_count;
        size_t vars_in_term = 0;
        for (auto v : *it) { (void)v; ++vars_in_term; }
        if (vars_in_term != 1) structure_ok = false;
    }
    structure_ok = structure_ok && (term_count == n);
#else
    size_t term_count = anf.raw().monomials().size();
    bool structure_ok = (term_count == n);
    for (const auto& mono : anf.raw().monomials()) {
        if (mono.size() != 1) structure_ok = false;
    }
#endif

    check(structure_ok, "tt_to_anf(parity_" + std::to_string(n) + "): структура ANF (" +
                             std::to_string(term_count) + " одночленов, ожидалось " +
                             std::to_string(n) + ", каждый из 1 переменной)");
}

}  // namespace

int main() {
    std::printf("=== verify/large_n_tests: независимая проверка n=25..%u (kMaxTruthTableVars) ===\n",
                kMaxTruthTableVars);

    // Полный перебор 2^n безопасен по времени до 2^25 (~34М точек, доли
    // секунды на builtin popcount-сравнение); дальше — выборка.
    constexpr uint64_t kExhaustiveLimit = uint64_t{1} << 25;

    for (uint32_t n : {20u, 24u, 25u, 28u, kMaxTruthTableVars}) {
        check_anf_to_tt(n, kExhaustiveLimit);
        check_tt_to_anf(n, kExhaustiveLimit);
    }

    // Явная проверка старой границы: n = kMaxTruthTableVars должен
    // работать (это и есть весь смысл её подъёма), n = kMaxTruthTableVars+1
    // обязан отказать с TooManyVariables, не упасть/зависнуть.
    {
        Anf anf = build_parity_anf(kMaxTruthTableVars + 1);
        auto result = anf_to_tt(anf);
        check(!is_ok(result) && error(result).code == ErrorCode::TooManyVariables,
              "anf_to_tt(parity_" + std::to_string(kMaxTruthTableVars + 1) +
                  "): корректно отказывает с TooManyVariables (n > kMaxTruthTableVars)");
    }

    std::printf("\n=== ИТОГ: %d проверок, %d провалов ===\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
