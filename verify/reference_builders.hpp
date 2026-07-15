// verify/reference_builders.hpp — независимые построители "эталонных" Aig/
// Bdd/Anf/Thr/TruthTable из verify::GroundTruthFunction, для теста функций
// трансляции, у которых source-формат — не TruthTable (см.
// verify/test_runner.hpp, параметр build_source).
//
// Не входит в зафиксированную структуру репозитория как отдельная точка (та
// называет только verify/{sat_encoding,metamorphic,ground_truth}/ и
// test_runner.hpp), но необходима, чтобы test_runner.hpp вообще мог получить
// исходное представление для growing_test_functions() без chicken-and-egg
// зависимости от самих функций трансляции: build_source() для aig_to_bdd не
// может быть tt_to_aig (это как раз одна из 20 функций, которую мы тестируем
// в других тестах, а раз этот файл — часть verify/, значит от него независим).
// Каждый builder построен механически, напрямую по таблице истинности через
// Shannon/Mёbius-разложение, без обращения к aig/bdd/anf/thr/*.cpp.
//
// Thr — исключение: не любая функция является пороговой, поэтому вместо
// "построй Thr по произвольной GroundTruthFunction" (в общем случае
// невозможно) ниже есть отдельный генератор growing_threshold_test_functions,
// значения которого истинны по построению через Thr::evaluate() (5 строк в
// core/common.hpp, инфраструктура, не код студентов).

#pragma once

#include <vector>

#include "core/common.hpp"
#include "verify/ground_truth/ground_truth.hpp"

namespace bmm::verify {

Result<TruthTable> reference_truth_table(const GroundTruthFunction& gt);

// Ограничено практически kMaxReferenceAigVars (см. sat_encoding.hpp) —
// дерево MUX растёт как 2^n. За пределом возвращает TooManyVariables, и
// test_runner.hpp корректно пропускает этот тестовый случай (не FAIL).
Result<Aig> reference_aig(const GroundTruthFunction& gt);

Result<Bdd> reference_bdd(const GroundTruthFunction& gt);

// См. предупреждение в core/CONVENTIONS.md п.5: если BMM_HAVE_BRIAL==0 (по
// умолчанию в текущем .devcontainer), используется AnfFallback-ветка Anf;
// BRiAl-ветка написана по документированным примитивам BoolePolyRing, но не
// проверена вживую — заголовков libbrial в текущем образе нет.
Result<Anf> reference_anf(const GroundTruthFunction& gt);

std::vector<Thr> growing_threshold_test_functions(uint32_t max_n = kMaxGroundTruthVars);

}  // namespace bmm::verify
