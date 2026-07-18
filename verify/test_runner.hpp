// verify/test_runner.hpp — общий движок прогона тестов ОДНОЙ функции
// трансляции. Используется из test_aig.cpp/test_bdd.cpp/test_anf.cpp/
// test_thr.cpp, по одному вызову run_translation_tests() на секцию.
//
// Не привязан к Catch2 намеренно: возвращает TestOutcome, а REQUIRE/секцию
// вокруг него оборачивает вызывающий test_<format>.cpp — так test_runner.hpp
// можно было бы использовать и с другим тестовым фреймворком, и (что более
// вероятно на практике) переиспользовать сам по себе в profiling/ или
// benchmarks/ скриптах, где Catch2 не нужен.
//
// Порядок проверок на каждую тестовую функцию из growing_test_functions():
//   1. ground_truth   — прямое сравнение результата перевода с эталонной
//                        таблицей истинности (verify/ground_truth/).
//   2. metamorphic    — Chow-параметры и (для n<=12) степень АНФ между
//                        исходным представлением и переводом должны совпасть
//                        (verify/metamorphic/), плюс проверка согласованности
//                        кофакторов x0/x1.
//   3. sat_encoding   — только когда То == Aig (см. TODO в
//                        verify/sat_encoding/sat_encoding.hpp): независимая
//                        структурная проверка эквивалентности через
//                        Tseitin+CaDiCaL/kissat.
// Первое расхождение на любом шаге -> FAIL с конкретным контрпримером.
// NotImplemented на пробном вызове -> SKIP, дальше не гоняем.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <type_traits>

#include "core/common.hpp"
#include "verify/ground_truth/ground_truth.hpp"
#include "verify/metamorphic/metamorphic.hpp"
#include "verify/sat_encoding/sat_encoding.hpp"

namespace bmm::verify {

enum class TestStatus { Pass, Fail, Skip };

inline const char* to_string(TestStatus s) {
    switch (s) {
        case TestStatus::Pass: return "PASS";
        case TestStatus::Fail: return "FAIL";
        case TestStatus::Skip: return "SKIP";
    }
    return "?";
}

struct TestOutcome {
    TestStatus status;
    std::string function_name;  // "aig_to_bdd" и т.п. — совпадает с именем секции в test_<format>.cpp
    std::string detail;
};

// Машиночитаемая строка для STATUS.md-генерации (verify/../CMakeLists.txt,
// таргет `status`, парсит эти строки grep'ом — см. README.md).
inline void print_status_line(const TestOutcome& outcome, std::ostream& os) {
    os << "BMM_STATUS " << outcome.function_name << " " << to_string(outcome.status) << " "
       << outcome.detail << "\n";
}

namespace detail {

inline uint64_t assignment_to_index(const std::vector<bool>& a) {
    uint64_t idx = 0;
    for (uint32_t i = 0; i < a.size(); ++i) {
        if (a[i]) idx |= (uint64_t{1} << i);
    }
    return idx;
}

inline Evaluator evaluator_from_gt(const GroundTruthFunction& gt) {
    return [&gt](const std::vector<bool>& a) { return gt.evaluate(assignment_to_index(a)); };
}

// Материализует r.to_tt() ОДИН раз и возвращает Evaluator с O(1)-подстановкой
// по битовой маске вместо повторного r.evaluate() на каждой из 2^n_vars
// точек. Ниже (run_translation_tests) evaluate() каждого представления и так
// вызывается ровно 2^n_vars раз — ОДИН этими самым to_tt() (или напрямую, для
// представлений, где to_tt() пока не реализован/недоступен) — а не до 4 раз
// (find_mismatch + compute_chow_parameters + compare_anf_degree +
// check_cofactor_commutativity), как было раньше. Особенно важно для Anf:
// Anf::evaluate() стоит O(размера полинома) за вызов (в отличие от, например,
// TruthTable::evaluate(), O(1)) — на плотных полиномах (OR/XOR-всех,
// случайные функции) при n_vars>=11-12 четыре независимых прохода по 2^n
// точкам каждый уже давали десятки секунд НА ОДНУ тестовую функцию, суммарно
// вплоть до зависания целой секции ctest. to_tt() сам эту же работу делает
// один раз; для Anf он же (core/anf_repr.hpp) использует настоящее
// преобразование Мёбиуса, O(n*2^n), а не наивный цикл по evaluate().
// Не нарушает независимость верификации (CONVENTIONS.md п.7): to_tt()/
// evaluate() — примитивы САМОГО типа представления (core/, не один из 20
// студенческих переводов), ровно то же доверие, что уже оказано
// TruthTable::evaluate()/Bdd::evaluate() и т.п. Evaluate() каждой точки
// по-прежнему реально вызывается (внутри to_tt()), просто один раз, а не
// четыре.
template <Representation R>
Evaluator make_fast_evaluator(const R& r) {
    auto tt_result = r.to_tt();
    if (is_ok(tt_result)) {
        TruthTable tt = value(tt_result);
        return [tt](const std::vector<bool>& a) {
            return kitty::get_bit(tt.raw(), assignment_to_index(a)) != 0;
        };
    }
    // to_tt() недоступен для этого n_vars (TooManyVariables) — сюда test_runner.hpp
    // не должен доходить при growing_test_functions()-размерах (<= kMaxTruthTableVars),
    // но на случай будущего расширения диапазона — честный fallback на прямой evaluate().
    return [&r](const std::vector<bool>& a) { return r.evaluate(a); };
}

}  // namespace detail

// From/To — любые два типа, удовлетворяющих Representation (core/common.hpp).
// translate — тестируемая функция трансляции (Result<To>(const From&)).
// build_source — как построить From для заданной GroundTruthFunction
//   НЕЗАВИСИМО от translate (см. verify/reference_builders.hpp) — если
//   построить не удаётся для конкретного n (например, reference_aig_from_table
//   практически ограничен kMaxReferenceAigVars), этот тестовый случай
//   пропускается, а не считается FAIL всей функции.
// test_functions — растущий набор тестовых функций (см.
//   verify::growing_test_functions() для источников TruthTable/Aig/Bdd/Anf;
//   для From=Thr нужен отдельный набор — не любая функция из общего списка
//   пороговая, см. verify::growing_threshold_test_functions() +
//   ground_truth_from_thr() ниже и test_thr.cpp).
template <Representation From, Representation To>
TestOutcome run_translation_tests(
    const std::string& function_name, const std::function<Result<To>(const From&)>& translate,
    const std::function<Result<From>(const GroundTruthFunction&)>& build_source,
    const std::vector<GroundTruthFunction>& test_functions) {
    if (test_functions.empty()) {
        return {TestStatus::Fail, function_name, "пустой набор тестовых функций"};
    }

    // Пробный вызов на первой (самой тривиальной) функции набора: если
    // translate ещё не реализован, дальше гонять весь растущий набор смысла
    // нет.
    {
        const auto& probe = test_functions.front();
        auto src = build_source(probe);
        if (!is_ok(src)) {
            return {TestStatus::Fail, function_name,
                    "build_source не смог построить даже тривиальный вход: " + error(src).message};
        }
        auto probe_result = translate(value(src));
        if (!is_ok(probe_result) && error(probe_result).code == ErrorCode::NotImplemented) {
            return {TestStatus::Skip, function_name, "функция ещё не реализована"};
        }
    }

    for (const auto& gt : test_functions) {
        auto src_result = build_source(gt);
        if (!is_ok(src_result)) continue;  // источник не построить на этом n — пропускаем случай, не функцию
        const From& src = value(src_result);

        auto dst_result = translate(src);
        if (!is_ok(dst_result)) {
            if (error(dst_result).code == ErrorCode::NotImplemented) {
                return {TestStatus::Skip, function_name, "функция ещё не реализована"};
            }
            // Для *_to_thr: ErrorCode::Unsupported на конкретном тестовом
            // случае ожидаем и корректен — не любая функция из
            // growing_test_functions() пороговая (например, XOR при n>=2
            // гарантированно не пороговая функция, это классический факт, а
            // не баг реализации). Такой случай просто пропускаем, не
            // проваливаем всю проверку функции.
            if constexpr (std::is_same_v<To, Thr>) {
                if (error(dst_result).code == ErrorCode::Unsupported) continue;
            }
            return {TestStatus::Fail, function_name,
                    "translate() вернул ошибку на '" + gt.name + "': " + error(dst_result).message};
        }
        const To& dst = value(dst_result);

        // eval_src/eval_dst материализуют to_tt() ОДИН раз каждый и переиспользуются
        // во всех шагах ниже (ground_truth + все metamorphic-проверки) вместо
        // повторного evaluate() на каждый шаг — см. detail::make_fast_evaluator.
        const Evaluator eval_src = detail::make_fast_evaluator(src);
        const Evaluator eval_dst = detail::make_fast_evaluator(dst);

        // --- шаг 1: ground_truth -------------------------------------------------
        auto mismatch = find_mismatch(gt, eval_dst);
        if (mismatch) {
            const auto assignment = decode_assignment(*mismatch, gt.n_vars);
            std::string bits;
            for (bool b : assignment) bits += (b ? '1' : '0');
            return {TestStatus::Fail, function_name,
                    "[ground_truth] '" + gt.name + "' разошлась в точке #" +
                        std::to_string(*mismatch) + " (x=" + bits + ")"};
        }

        // --- шаг 2: metamorphic ---------------------------------------------------

        const auto chow_src = compute_chow_parameters(gt.n_vars, eval_src);
        const auto chow_dst = compute_chow_parameters(gt.n_vars, eval_dst);
        if (auto cm = compare_chow_parameters(chow_src, chow_dst)) {
            return {TestStatus::Fail, function_name,
                    "[metamorphic/chow] '" + gt.name + "': " +
                        (cm->weight_mismatch
                             ? "вес функции (a0) разошёлся"
                             : "корреляция с x" + std::to_string(*cm->first_mismatched_var) +
                                   " разошлась")};
        }

        // Мёбиус-транформ — O(n * 2^n); ограничиваем n ради скорости CI, это
        // не влияет на покрытие ground_truth (тот уже проверил все точки).
        if (gt.n_vars <= 12) {
            if (auto dm = compare_anf_degree(gt.n_vars, eval_src, eval_dst)) {
                return {TestStatus::Fail, function_name,
                        "[metamorphic/anf_degree] '" + gt.name + "': степень АНФ " +
                            std::to_string(dm->degree_a) + " (src) vs " +
                            std::to_string(dm->degree_b) + " (dst)"};
            }
        }

        if (gt.n_vars >= 2) {
            if (auto cf = check_cofactor_commutativity(gt.n_vars, eval_src, eval_dst, 0, 1)) {
                return {TestStatus::Fail, function_name,
                        "[metamorphic/cofactor] '" + gt.name + "': x" + std::to_string(cf->var_i) +
                            "=" + (cf->vi ? "1" : "0") + ", x" + std::to_string(cf->var_j) + "=" +
                            (cf->vj ? "1" : "0") + ", свободная точка #" +
                            std::to_string(cf->free_index)};
            }
        }

        // --- шаг 3: sat_encoding (только То == Aig, см. TODO в sat_encoding.hpp) --
        if constexpr (std::is_same_v<To, Aig>) {
            if (gt.n_vars <= kMaxReferenceAigVars) {
                const auto reference = reference_aig_from_table(gt.n_vars, detail::evaluator_from_gt(gt));
                const auto eq = check_aig_equivalence_via_sat(reference, dst.raw());
                if (eq.checked && !eq.equivalent) {
                    return {TestStatus::Fail, function_name,
                            "[sat_encoding] '" + gt.name + "': солвер нашёл контрпример (miter SAT)"};
                }
            }
        }
    }

    std::string steps = "ground_truth + metamorphic";
    if constexpr (std::is_same_v<To, Aig>) steps += " + sat_encoding";
    return {TestStatus::Pass, function_name, "все проверки пройдены (" + steps + ")"};
}

// Удобный overload для source-форматов TruthTable/Aig/Bdd/Anf: набор
// тестовых функций — общий growing_test_functions(max_n). Для From=Thr этот
// overload использовать НЕЛЬЗЯ (см. пояснение выше и ground_truth_from_thr
// ниже) — не любая функция из growing_test_functions() пороговая, а Thr не
// умеет представлять произвольную булеву функцию.
template <Representation From, Representation To>
TestOutcome run_translation_tests(
    const std::string& function_name, const std::function<Result<To>(const From&)>& translate,
    const std::function<Result<From>(const GroundTruthFunction&)>& build_source,
    uint32_t max_n = kMaxGroundTruthVars) {
    static_assert(!std::is_same_v<From, Thr>,
                  "run_translation_tests<Thr, To> требует явный набор тестовых функций "
                  "через ground_truth_from_thr(growing_threshold_test_functions(...)) — "
                  "см. test_thr.cpp");
    return run_translation_tests<From, To>(function_name, translate, build_source,
                                            growing_test_functions(max_n));
}

// Материализует Thr в GroundTruthFunction через его собственный evaluate()
// (5-строчная сумма в core/common.hpp, инфраструктура, не код студентов —
// тот же уровень доверия, что и kitty-based TruthTable::evaluate() в других
// builder'ах verify/reference_builders.hpp). Нужен, чтобы From=Thr тесты
// могли использовать общий run_translation_tests(..., test_functions).
inline GroundTruthFunction ground_truth_from_thr(const Thr& thr, const std::string& name) {
    GroundTruthFunction gt;
    gt.name = name;
    gt.n_vars = thr.n_vars();
    const uint64_t rows = uint64_t{1} << gt.n_vars;
    gt.table.resize(rows);
    for (uint64_t idx = 0; idx < rows; ++idx) {
        gt.table[idx] = thr.evaluate(decode_assignment(idx, gt.n_vars));
    }
    return gt;
}

}  // namespace bmm::verify
