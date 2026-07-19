// benchmarks/cnf_dimacs_loader.hpp — загрузчик DIMACS CNF (benchmarks/data/
// dimacs-cnf/*.cnf, benchmarks/data/satlib/<категория>/*.cnf) в bmm::Aig.
//
// Формат DIMACS CNF (стандартный SAT Competition, тот же, что уже пишет
// verify/sat_encoding::solve_dimacs в обратную сторону):
//   c <комментарий>                (ноль и более строк, игнорируются)
//   p cnf <n_vars> <n_clauses>     (ровно одна строка-заголовок)
//   <lit1> <lit2> ... <litk> 0     (по одной строке на клозу; литерал —
//                                   ненулевое целое, |lit| — номер переменной
//                                   1..n_vars, знак — полярность;
//                                   терминатор 0)
//
// <n_clauses> в заголовке не проверяется на равенство фактическому числу
// прочитанных клоз (тот же необязательный статус, что у аналогичного поля в
// benchmarks/anf_dimacs_loader.hpp).
//
// cnf_to_aig строит AIG, вычисляющий ИНДИКАТОР выполнимости: 1 на
// присвоении переменных ровно тогда, когда оно удовлетворяет ВСЕМ клозам
// CNF. Это НЕ Tseitin-кодирование (которое добавляет вспомогательные
// переменные для СОХРАНЕНИЯ эквисатисфиability при разметке промежуточных
// подформул — см. verify/sat_encoding, обратное направление AIG->CNF) — в
// обратную сторону, раз AIG требует ОДИН PO (core/CONVENTIONS.md п.4),
// естественная и единственная корректная цель — прямая индикаторная функция
// самой формулы: CNF = AND по клозам, клоза = OR по литералам, ровно то, что
// значит "формула выполнена". mockturtle::aig_network::create_nary_or/
// create_nary_and уже реализуют это стандартными gate-примитивами (De
// Morgan: OR через AND, см. mockturtle/networks/aig.hpp) — здесь только
// разбор формата и переиспользование bmm::verify::CnfFormula (общий тип с
// verify/sat_encoding, чтобы результат парсинга сразу годился и для
// независимой проверки через solve_dimacs/внешний SAT-солвер).

#pragma once

#include "core/common.hpp"
#include "verify/sat_encoding/sat_encoding.hpp"

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace bmm::benchmarks {

inline std::optional<verify::CnfFormula> load_cnf_dimacs(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;

    verify::CnfFormula cnf;
    bool have_header = false;
    uint32_t n_clauses_declared = 0;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == 'c') continue;

        if (!have_header) {
            if (line[0] != 'p') return std::nullopt;
            std::istringstream iss(line);
            std::string tag, fmt;
            iss >> tag >> fmt >> cnf.num_vars >> n_clauses_declared;
            if (!iss || tag != "p" || fmt != "cnf") return std::nullopt;
            have_header = true;
            cnf.clauses.reserve(n_clauses_declared);
            continue;
        }

        std::istringstream iss(line);
        verify::CnfClause clause;
        int lit = 0;
        bool any_token = false;
        while (iss >> lit) {
            any_token = true;
            if (lit == 0) break;
            const uint32_t var = static_cast<uint32_t>(lit < 0 ? -lit : lit);
            if (var < 1 || var > cnf.num_vars) return std::nullopt;
            clause.literals.push_back(lit);
        }
        if (!any_token) continue;  // пустая/пробельная строка после заголовка
        cnf.clauses.push_back(std::move(clause));
    }
    if (!have_header) return std::nullopt;

    return cnf;
}

// Строит AIG-индикатор выполнимости (см. пояснение в шапке файла). PI i
// (i=0..n_vars-1) соответствует DIMACS-переменной (i+1) — та же 1-индексация
// в файле -> 0-индексация внутри Aig, что и в anf_dimacs_loader.hpp.
inline Aig cnf_to_aig(const verify::CnfFormula& cnf) {
    mockturtle::aig_network net;

    std::vector<mockturtle::aig_network::signal> pis(cnf.num_vars);
    for (uint32_t i = 0; i < cnf.num_vars; ++i) pis[i] = net.create_pi();

    std::vector<mockturtle::aig_network::signal> clause_signals;
    clause_signals.reserve(cnf.clauses.size());

    for (const auto& clause : cnf.clauses) {
        std::vector<mockturtle::aig_network::signal> lits;
        lits.reserve(clause.literals.size());
        for (int lit : clause.literals) {
            const uint32_t var = static_cast<uint32_t>(lit < 0 ? -lit : lit) - 1;
            lits.push_back(lit < 0 ? !pis[var] : pis[var]);
        }
        // Пустая клоза (0 литералов до терминатора) — create_nary_or({}) даёт
        // get_constant(false) (см. tree_reduce в aig.hpp): корректно —
        // невыполнимая клоза делает невыполнимой всю формулу.
        clause_signals.push_back(net.create_nary_or(lits));
    }

    // Пустая CNF (0 клоз) — create_nary_and({}) даёт get_constant(true):
    // корректно, пустая конъюнкция выполнена вакуумно.
    net.create_po(net.create_nary_and(clause_signals));

    return Aig(std::move(net));
}

}  // namespace bmm::benchmarks
