#pragma once

#include "core/anf_repr.hpp"
#include "core/bdd_order_heuristics.hpp"

namespace bmm {

// anf_to_bdd — построение BDD из полинома Жегалкина (ANF).
//
// Вход:  Anf.
// Выход: Bdd (sylvan::Bdd).
//
// Алгоритм/литература: рекурсивная кофакторная схема, зеркальная к
// bdd_to_anf (bdd/bdd_to_anf.hpp) и опирающаяся на то же тождество, только
// в обратную сторону: разложите ANF по переменной x_i как
// p = p0 XOR x_i * p1 (p0 — сумма мономов без x_i, p1 — сумма мономов с
// x_i, после "снятия" x_i), тогда p0 = p|x_i=0, p1 = p|x_i=1 XOR p|x_i=0,
// и соответствующий узел BDD — Ite(bddVar(i), BDD(p|x_i=1), BDD(p|x_i=0)).
// Рекурсия по мономам (сгруппированным по вхождению x_i) вместо перебора
// точек — так асимптотика зависит от числа мономов, а не от 2^n напрямую.
//
// Параллелизм: core/CONVENTIONS.md п.6, правило 1 — Bdd является выходом,
// поэтому построение BDD-узлов — только через sylvan::Bdd::Ite/операторы
// (Sylvan/Lace), без собственного TBB/OpenMP. Разбиение мономов на группы
// "содержит x_i"/"не содержит x_i" можно делать последовательно перед тем,
// как вызывать Sylvan.
//
// Порядок переменных (сравнение эвристик — см. подробную историю находок в
// anf_to_bdd.cpp): физический уровень Sylvan для узла фиксирован ИНДЕКСОМ
// переменной, поэтому единственный способ получить действительно другой
// порядок переменных в итоговом BDD, а не только другой порядок обхода
// рекурсии — строить узлы с bddVar(order[var]) и вернуть Bdd с явным
// var_to_level = order (см. Bdd(root, n_vars, var_to_level) в
// core/common.hpp). anf_to_bdd(anf) использует эвристику, выбранную по
// итогам боевого сравнения (см. комментарий у convert() в .cpp);
// anf_to_bdd_with_heuristic() — та же функция с явным выбором эвристики,
// для сравнительных бенчмарков (anf/bench_bdd_heuristics.cpp) и на будущее,
// если понадобится сравнение вне бенчмарка. Внешний production-код должен
// использовать anf_to_bdd(anf).
//
// VariableOrderHeuristic — общий для anf_to_bdd и aig_to_bdd тип (см.
// core/bdd_order_heuristics.hpp): MinIndex/Degree/Force не знают о мономах,
// только о графе взаимодействия переменных, который каждый производитель
// строит по своим данным (anf_to_bdd.cpp — по мономам, aig_to_bdd.cpp — по
// фанинам AND-гейтов). LengthFreqRank — исключение, ANF-специфична
// (использует понятие "длина монома"), её расчёт остался в anf_to_bdd.cpp.
Result<Bdd> anf_to_bdd_with_heuristic(const Anf& anf, VariableOrderHeuristic heuristic);

// Тесты: test_anf.cpp, секция "anf_to_bdd".
Result<Bdd> anf_to_bdd(const Anf& anf);

}  // namespace bmm
