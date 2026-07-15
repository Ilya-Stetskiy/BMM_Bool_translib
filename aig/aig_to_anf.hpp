#pragma once

#include "core/common.hpp"

namespace bmm {

// aig_to_anf — построение полинома Жегалкина (ANF), вычисляющего ту же
// функцию, что и AIG.
//
// Вход:  Aig (mockturtle::aig_network, один PO).
// Выход: Anf (BoolePolynomial при BMM_HAVE_BRIAL, иначе AnfFallback — см.
//        core/CONVENTIONS.md п.5).
//
// Алгоритм/литература:
//   AIG состоит только из AND и инверсии, а в кольце GF(2)[x]/(x_i^2-x_i)
//   (в котором живёт ANF) это ровно умножение и XOR с константой 1
//   (NOT(a) = 1 XOR a). Поэтому ANF получается прямой алгебраической
//   трансляцией AIG-узлов: AND-узел -> произведение двух полиномов фанинов
//   (раскрытое по дистрибутивности и приведённое по модулю x_i^2=x_i),
//   инверсия ребра -> XOR результата с константой 1. См. Carlet, "Boolean
//   Functions for Cryptography and Error-Correcting Codes" (в сб. "Boolean
//   Models and Methods...", 2010), раздел про ANF/полином Жегалкина булевых
//   схем. Существенная сложность — раскрытие произведения двух полиномов
//   в общем случае может дать экспоненциальный рост числа мономов
//   (это свойство ANF, а не недостаток алгоритма); мемоизация одинаковых
//   промежуточных полиномов по узлам AIG обязательна для приемлемой
//   производительности.
//
// Параллелизм: core/CONVENTIONS.md п.6, правило 2 — TBB, task_group на
// обход узлов AIG + concurrent_hash_map для мемоизации node -> Anf.
//
// Обязательный бенчмарк (вторая итерация задания): TBB-функция, реализация
// ДОЛЖНА пройти секцию "aig_to_anf_tbb_scaling" в test_aig.cpp (замер
// 1-поточного vs полного tbb::global_control на n=8/12/kMaxReferenceAigVars,
// см. benchmarks/tbb_scaling.hpp) — результат идёт в STATUS.md рядом со
// статусом функции.
//
// Тесты: test_aig.cpp, секции "aig_to_anf" (корректность) и
// "aig_to_anf_tbb_scaling" (обязательный бенчмарк параллельности).
Result<Anf> aig_to_anf(const Aig& aig);

}  // namespace bmm
