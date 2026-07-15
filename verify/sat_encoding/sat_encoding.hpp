// verify/sat_encoding/sat_encoding.hpp — Tseitin-энкодинг + внешний SAT-солвер
// как независимый путь верификации эквивалентности.
//
// MVP-скоуп (см. CONVENTIONS.md core/ п.7 и STATUS.md для статуса):
//  - Реализовано полностью: Tseitin-кодирование AIG (mockturtle::aig_network)
//    в CNF, запись DIMACS, вызов cadical/kissat как внешнего процесса
//    (в контейнере есть только бинарники, не libcadical.a/cadical.hpp — см.
//    core/CONVENTIONS.md п.7), проверка эквивалентности через miter (XOR
//    выходов) + UNSAT-проверку.
//  - reference_aig_from_table строит "эталонный" AIG механически по таблице
//    истинности через рекурсивное Shannon/MUX-разложение — самостоятельный
//    код, не переиспользующий ни одну из 20 функций трансляции. Экспоненциален
//    по построению (2^n gates), поэтому практически пригоден только для
//    n <= kMaxReferenceAigVars — это осознанное ограничение MVP, а не баг.
//  - TODO (не реализовано, честно помечено в STATUS.md, а не тихо пропущено):
//    структурные Tseitin-энкодеры для Bdd (ITE-энкодинг узлов), Anf
//    (XOR/AND-раскрытие монома) и Thr (кардинальное ограничение —
//    sequential-counter или сортирующая сеть в духе Eén–Sörensson 2006).
//    Без них SAT-путь верификации сейчас покрывает только представления,
//    которые уже переведены в Aig (напрямую или как промежуточный шаг).

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <mockturtle/networks/aig.hpp>

namespace bmm::verify {

// Практический потолок для reference_aig_from_table: дерево MUX растёт как
// 2^n узлов, при n=14 это уже ~16K gates — приемлемо для CI, дальше не
// стоит без мемоизации по под-таблицам (не реализовано, см. TODO выше).
inline constexpr uint32_t kMaxReferenceAigVars = 14;

struct CnfClause {
    std::vector<int> literals;  // DIMACS-литералы: +v/-v, v начинается с 1
};

struct CnfFormula {
    uint32_t num_vars = 0;
    std::vector<CnfClause> clauses;
};

// Один литерал (переменная + знак) на узел AIG (включая PI и константу),
// плюс список литералов PO в порядке foreach_po. Используется для построения
// miter'а в check_aig_equivalence_via_sat.
struct AigCnfEncoding {
    CnfFormula cnf;
    std::vector<int> po_literals;
};

AigCnfEncoding encode_aig_tseitin(const mockturtle::aig_network& aig);

// Механическое построение AIG по таблице истинности (Shannon/MUX-разложение,
// только create_pi/create_and/create_po/get_constant + отрицание сигнала) —
// независимый эталон, см. пояснение в шапке файла.
mockturtle::aig_network reference_aig_from_table(
    uint32_t n_vars, const std::function<bool(const std::vector<bool>&)>& f);

struct SatResult {
    bool solver_ran = false;         // false, если бинарник солвера не найден в PATH
    bool satisfiable = false;        // валиден только при solver_ran == true
    std::optional<std::vector<bool>> witness;  // значения переменных 1..num_vars, если satisfiable
};

// Пишет CNF во временный DIMACS-файл в scratch-директории, вызывает
// solver_binary как внешний процесс, разбирает exit code (10=SAT, 20=UNSAT,
// SAT Competition convention) и, если SAT, строки "v ..." из stdout.
SatResult solve_dimacs(const CnfFormula& cnf, const std::string& solver_binary = "cadical");

struct EquivalenceCheckResult {
    bool checked = false;   // false, если солвер недоступен — не значит FAIL, см. test_runner.hpp
    bool equivalent = false;
    std::optional<std::vector<bool>> counterexample;  // присвоение PI, на котором выходы разошлись
};

// miter = XOR(po_a, po_b) для соответствующих PO; UNSAT(miter=1) <=>
// функции эквивалентны. Предполагается одинаковое число PI в a и b в одном и
// том же порядке (что гарантировано, если оба построены из одной and той же
// таблицы истинности/присвоения).
EquivalenceCheckResult check_aig_equivalence_via_sat(const mockturtle::aig_network& a,
                                                      const mockturtle::aig_network& b,
                                                      const std::string& solver_binary = "cadical");

}  // namespace bmm::verify
