#pragma once

#include "core/common.hpp"

namespace bmm {

// aig_to_tt — полная симуляция AIG на всех 2^n входах, построение таблицы
// истинности.
//
// Вход:  Aig (mockturtle::aig_network, один PO), n_vars() <=
//        kMaxTruthTableVars (иначе верните ErrorCode::TooManyVariables —
//        уже реализовано в bmm::Aig::to_tt(), см. core/common.hpp; эта
//        функция — самостоятельная реализация того же контракта в
//        production-коде aig/, Aig::to_tt() из core/ не вызывайте отсюда,
//        она инфраструктурная и намеренно используется только в verify/).
// Выход: TruthTable.
//
// Алгоритм/литература: тривиальная полная симуляция — для каждой из 2^n
// точек вычислить значение AIG (топологический проход по узлам,
// evaluate(node) = AND(evaluate(fanin0), evaluate(fanin1)) с учётом
// инверсий). Единственная реальная забота — эффективность: для n близких к
// kMaxTruthTableVars=24 это 16M точек x размер AIG операций.
//
// Параллелизм: core/CONVENTIONS.md п.6, правило 3 (выход — плоский TT) —
// OpenMP `#pragma omp parallel for` по индексу точки 0..2^n-1, результат
// пишется в плоский `uint64_t[]`/`bool[]`-буфер (без объектов/указателей в
// теле цикла — задел под перенос на GPU, см. profiling/README.md). AIG сам
// по себе можно either симулировать point-by-point на каждом потоке, либо
// (эффективнее) провести побитовую SIMD-симуляцию словами по 64 точки за
// раз — выбор за студентом.
//
// Тесты: test_aig.cpp, секция "aig_to_tt".
Result<TruthTable> aig_to_tt(const Aig& aig);

}  // namespace bmm
