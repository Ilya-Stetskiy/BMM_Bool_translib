#pragma once

#include "core/common.hpp"

namespace bmm {

// tt_to_thr — распознавание, что функция, заданная таблицей истинности,
// является пороговой (threshold), и построение представления Thr.
//
// Вход:  TruthTable, n_vars() <= kMaxTruthTableVars.
// Выход: Thr (веса w_1..w_n + порог theta).
//
// Алгоритм/литература: Muroga, S., "Threshold Logic and Its Applications",
// Wiley, 1971 — классический точный подход. Задача сводится к LP/ILP: для
// каждой точки x с f(x)=1 — неравенство sum(w_i*x_i) >= theta, для каждой
// точки с f(x)=0 — sum(w_i*x_i) < theta (или <= theta-1 для целых весов);
// система из 2^n неравенств на n+1 неизвестных (w_1..w_n, theta) разрешима
// тогда и только тогда, когда функция пороговая. LP/ILP-солвер — Google
// OR-Tools (`ortools::ortools`, см. core/CONVENTIONS.md п.5а, `bmm_thr` уже
// его линкует); для малых n перебор целочисленных весов в ограниченном
// диапазоне тоже остаётся валидным более простым вариантом. Chow-параметры
// (verify/metamorphic/metamorphic.hpp::compute_chow_parameters) дают
// быстрый необходимый фильтр, которым можно дёшево отбросить заведомо
// непороговые функции перед дорогим точным шагом.
//
// Если функция не является пороговой — верните ErrorCode::Unsupported (не
// NotImplemented).
//
// Параллелизм: core/CONVENTIONS.md п.6, правило 3 — OpenMP `parallel for`
// уместен на этапе перебора точек таблицы истинности для построения
// системы неравенств (плоский проход по 2^n записям); сам LP/ILP-решатель
// обычно последовательный.
//
// Тесты: test_thr.cpp, секция "tt_to_thr".
Result<Thr> tt_to_thr(const TruthTable& tt);

}  // namespace bmm
