#pragma once

#include "core/common.hpp"

namespace bmm {

// aig_to_bdd — построение BDD, вычисляющего ту же функцию, что и AIG.
//
// Вход:  Aig (mockturtle::aig_network, один PO).
// Выход: Bdd (обёртка sylvan::Bdd, см. core/common.hpp).
//
// Алгоритм/литература:
//   Стандартное построение BDD по произвольной схеме (Bryant, "Graph-Based
//   Algorithms for Boolean Function Manipulation", 1986): топологический
//   обход AND-узлов AIG от PI к PO, на каждом узле — sylvan::Bdd AND двух
//   уже построенных BDD фанинов (с учётом инверсии через оператор `!` у
//   sylvan::Bdd), результат PI — sylvan::Bdd::bddVar(i). Мемоизация node ->
//   Bdd обязательна: без неё сложность экспоненциальна по глубине AIG даже
//   притом что Sylvan сам дедуплицирует BDD-узлы внутри.
//
// Параллелизм: core/CONVENTIONS.md п.6, правило 1 — Bdd является выходом,
// поэтому НИКАКОГО собственного TBB/OpenMP здесь, только Sylvan/Lace API.
// Обход AIG (топологическая сортировка узлов) можно делать последовательно
// одним потоком — сами BDD-операции (`&`, `!`) Sylvan распараллеливает
// внутри себя через Lace, если вызывать их из зарегистрированного
// Lace-потока (обычно это делает `RUN(...)`/`sylvan::Bdd` уже сам по себе
// внутри активного Sylvan-контекста, инициализированного в main()).
//
// Тесты: test_aig.cpp, секция "aig_to_bdd".
Result<Bdd> aig_to_bdd(const Aig& aig);

}  // namespace bmm
