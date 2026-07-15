// verify/metamorphic/metamorphic.hpp — метаморфные инварианты.
//
// Инварианты сформулированы над парой (n_vars, evaluate-функция), а не над
// bmm::Representation напрямую: это (а) не тянет зависимость на
// core/common.hpp вообще, оставаясь независимым путём верификации, и
// (б) позволяет test_runner.hpp склеивать один и тот же код проверки что с
// исходным представлением, что с результатом перевода, что с ground_truth
// эталоном — без дублирования.
//
// Метаморфное тестирование здесь означает: одно и то же свойство функции
// проверяется ДВУМЯ разными вычислительными путями (прямой перебор точек —
// в ground_truth/; здесь — через Chow-параметры, степень АНФ по преобразованию
// Мёбиуса и через явную реструктуризацию вычисления кофакторами), так что
// баг именно в самом evaluate()/to_tt() конкретного представления, который
// случайно "переживает" прямое сравнение таблиц (например, из-за одинаковой
// ошибки в обоих направлениях), с большей вероятностью проявится хотя бы в
// одном из альтернативных путей.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bmm::verify {

inline constexpr uint32_t kMaxMetamorphicVars = 20;

using Evaluator = std::function<bool(const std::vector<bool>&)>;

// ---------------------------------------------------------------------------
// Chow parameters: (a_0, a_1, ..., a_{n-1}), a_0 = |f^{-1}(1)|,
// a_i = sum_{x: f(x)=1} x_i. Инвариант функции: если два evaluate() считают
// одну и ту же булеву функцию, их Chow-параметры совпадают побитово-точно
// (это прямое следствие определения, не эвристика) — расхождение однозначно
// указывает на разные функции и (через то, какие именно a_i разошлись) даёт
// зацепку, в какой переменной проблема.
// ---------------------------------------------------------------------------

struct ChowParameters {
    uint64_t weight = 0;                  // a_0
    std::vector<uint64_t> correlations;    // a_1..a_n, correlations[i] = a_{i+1}
};

ChowParameters compute_chow_parameters(uint32_t n_vars, const Evaluator& f);

struct ChowMismatch {
    bool weight_mismatch;
    std::optional<uint32_t> first_mismatched_var;  // индекс i, если weight совпал, но a_i разошлись
};

// std::nullopt, если параметры совпали.
std::optional<ChowMismatch> compare_chow_parameters(const ChowParameters& a,
                                                     const ChowParameters& b);

// ---------------------------------------------------------------------------
// Степень АНФ через преобразование Мёбиуса (быстрое XOR-версия zeta/mobius
// transform, O(n * 2^n), стандартный алгоритм построения полинома Жегалкина
// из таблицы истинности) — независимая от anf/*.cpp реализация: не
// использует ни BRiAl, ни AnfFallback из core/common.hpp, тестирует
// исключительно evaluate().
// ---------------------------------------------------------------------------

// Коэффициенты монома по битовой маске переменных, входящих в моном
// (coefficients[mask] — коэффициент монома prod_{i: mask бит i =1} x_i).
std::vector<bool> mobius_transform(uint32_t n_vars, const Evaluator& f);

// Степень АНФ = максимальный popcount(mask) среди mask с ненулевым
// коэффициентом (0, если функция тождественно равна 0).
uint32_t anf_degree(uint32_t n_vars, const Evaluator& f);

struct DegreeMismatch {
    uint32_t degree_a;
    uint32_t degree_b;
};

std::optional<DegreeMismatch> compare_anf_degree(uint32_t n_vars, const Evaluator& a,
                                                  const Evaluator& b);

// ---------------------------------------------------------------------------
// Согласованность кофакторов: f|x_i=vi|x_j=vj (в любом порядке фиксации)
// должно быть одной и той же под-функцией от оставшихся n-2 переменных, и
// эта под-функция должна совпадать между evaluate_a и evaluate_b (двумя
// представлениями одной и той же булевой функции — обычно "до" и "после"
// перевода). Проверка идёт через отдельный от find_mismatch путь построения
// присвоения (restrict_and_reindex ниже), а не через прямой перебор всех
// n-битных точек.
// ---------------------------------------------------------------------------

struct CofactorMismatch {
    uint32_t var_i, var_j;
    bool vi, vj;
    uint64_t free_index;  // индекс точки среди оставшихся n-2 свободных переменных
};

std::optional<CofactorMismatch> check_cofactor_commutativity(uint32_t n_vars, const Evaluator& a,
                                                               const Evaluator& b, uint32_t var_i,
                                                               uint32_t var_j);

}  // namespace bmm::verify
