#pragma once

#include "core/bdd_order_heuristics.hpp"
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
// Порядок переменных: как и anf_to_bdd (см. anf/anf_to_bdd.hpp), физический
// уровень Sylvan для узла фиксирован ИНДЕКСОМ переменной — единственный
// способ получить действительно другой (не натуральный) порядок в итоговом
// BDD — строить узлы с bddVar(rank[var]) и вернуть Bdd с явным var_to_level.
// Натуральный (по индексу PI, MinIndex) порядок не имеет защиты от
// экспоненциального взрыва на входах с "неудачной" структурой графа
// взаимодействия переменных — тот же класс риска, что задокументирован для
// anf_to_bdd в anf/README.md §5.2, подтверждён эмпирически и для aig_to_bdd
// на реальной схеме (EPFL router.aig, n=60: натуральный порядок вызывал
// зависание построения BDD — см. SESSION_REPORT.md §8).
//
// ИСПРАВЛЕНО (было расхождение между этим doc-комментарием и кодом —
// см. SESSION_REPORT.md §8/aig_to_bdd.cpp): aig_to_bdd(aig) БЕЗ суффикса
// _with_heuristic уже по умолчанию использует Force, а не натуральный
// MinIndex-порядок — именно это и закрывает риск выше на реальных схемах
// "из коробки", без явного выбора эвристики вызывающей стороной.
// aig_to_bdd_with_heuristic(aig, heuristic) строит граф взаимодействия
// переменных по фанин-структуре AIG (см. aig_to_bdd.cpp) — используйте её
// явно только если нужна КОНКРЕТНАЯ эвристика (например MinIndex — самый
// дешёвый путь там, где переупорядочивание заведомо не нужно, или для
// сравнительных бенчмарков); для обычного использования достаточно
// aig_to_bdd(aig).
Result<Bdd> aig_to_bdd_with_heuristic(const Aig& aig, VariableOrderHeuristic heuristic);

// Тесты: test_aig.cpp, секция "aig_to_bdd". aig_to_bdd(aig) — это
// aig_to_bdd_with_heuristic(aig, VariableOrderHeuristic::Force), см. выше.
Result<Bdd> aig_to_bdd(const Aig& aig);

}  // namespace bmm
