#pragma once

#include "core/common.hpp"

namespace bmm {

// tt_to_bdd — построение BDD из таблицы истинности.
//
// Вход:  TruthTable, n_vars() <= kMaxTruthTableVars.
// Выход: Bdd (sylvan::Bdd, см. core/common.hpp).
//
// Алгоритм/литература: классическое рекурсивное построение по Shannon-
// разложению (Bryant, 1986) — f = ITE(x_i, f|x_i=1, f|x_i=0), листья —
// значения таблицы истинности. sylvan::Bdd::Ite(then, else) — готовый
// примитив для узла разложения; sylvan::Bdd::bddVar(i) — переменная.
// Sylvan сам дедуплицирует одинаковые поддеревья через unifying table,
// поэтому явная мемоизация на уровне вашего кода не обязательна (в отличие
// от AIG/ANF-построения), но естественная граница рекурсии — совпадающие
// диапазоны индексов таблицы истинности — всё равно стоит учитывать для
// производительности при больших n.
//
// Параллелизм: core/CONVENTIONS.md п.6, правило 1 — Bdd является выходом,
// значит только Sylvan/Lace, никакого собственного TBB/OpenMP.
//
// Тесты: test_bdd.cpp, секция "tt_to_bdd".
Result<Bdd> tt_to_bdd(const TruthTable& tt);

}  // namespace bmm
