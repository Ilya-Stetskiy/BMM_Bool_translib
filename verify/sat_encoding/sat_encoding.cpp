#include "sat_encoding.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>

#if defined(__unix__) || defined(__APPLE__)
#  include <sys/wait.h>
#  include <unistd.h>
#  define BMM_HAVE_POSIX_WAIT 1
#else
#  define BMM_HAVE_POSIX_WAIT 0
#endif

namespace bmm::verify {

namespace {

// Один Tseitin-литерал (CNF-переменная + знак) на узел AIG. Индексация:
// переменная node_var[node] закреплена за узлом; знак берётся из
// mockturtle-сигнала (aig.is_complemented(signal)) в момент использования —
// сама by CNF-переменная у узла всегда одна и та же независимо от полярности
// сигналов, которые на неё ссылаются.
int node_literal(const mockturtle::aig_network& aig, mockturtle::aig_network::signal s,
                  const std::vector<int>& node_var) {
    const int v = node_var[aig.node_to_index(aig.get_node(s))];
    return aig.is_complemented(s) ? -v : v;
}

}  // namespace

AigCnfEncoding encode_aig_tseitin(const mockturtle::aig_network& aig) {
    AigCnfEncoding result;

    // Переменная 1 всегда зарезервирована за константным узлом (node 0 в
    // mockturtle), принудительно false юнит-клозой — обычная Tseitin-практика.
    std::vector<int> node_var(aig.size(), 0);
    int next_var = 1;
    node_var[aig.node_to_index(aig.get_node(aig.get_constant(false)))] = next_var++;

    aig.foreach_pi([&](auto node) { node_var[aig.node_to_index(node)] = next_var++; });
    aig.foreach_gate([&](auto node) { node_var[aig.node_to_index(node)] = next_var++; });

    result.cnf.num_vars = static_cast<uint32_t>(next_var - 1);

    // Константа: unit-клоза, запрещающая true.
    result.cnf.clauses.push_back(
        {{-node_var[aig.node_to_index(aig.get_node(aig.get_constant(false)))]}});

    // Tseitin для AND-узла g = f0 AND f1 (f0/f1 — литералы фанинов с учётом
    // их полярности): (¬g∨f0) ∧ (¬g∨f1) ∧ (g∨¬f0∨¬f1).
    aig.foreach_gate([&](auto node) {
        const int g = node_var[aig.node_to_index(node)];
        std::array<int, 2> fanins{};
        uint32_t k = 0;
        aig.foreach_fanin(node, [&](auto signal) { fanins[k++] = node_literal(aig, signal, node_var); });
        const int f0 = fanins[0];
        const int f1 = fanins[1];
        result.cnf.clauses.push_back({{-g, f0}});
        result.cnf.clauses.push_back({{-g, f1}});
        result.cnf.clauses.push_back({{g, -f0, -f1}});
    });

    aig.foreach_po([&](auto signal) { result.po_literals.push_back(node_literal(aig, signal, node_var)); });

    return result;
}

mockturtle::aig_network reference_aig_from_table(
    uint32_t n_vars, const std::function<bool(const std::vector<bool>&)>& f) {
    mockturtle::aig_network aig;
    std::vector<mockturtle::aig_network::signal> pis(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i) pis[i] = aig.create_pi();

    // OR(a,b) = ¬(¬a ∧ ¬b), AND — единственный примитивный гейт AIG.
    auto aig_or = [&](auto a, auto b) { return !aig.create_and(!a, !b); };
    auto aig_mux = [&](auto sel, auto hi, auto lo) {
        return aig_or(aig.create_and(sel, hi), aig.create_and(!sel, lo));
    };

    // Рекурсивное Shannon-разложение по x_0..x_{n-1}: build(0) — весь MUX-дерево.
    // Независимая реализация — см. пояснение в sat_encoding.hpp.
    std::vector<bool> assignment(n_vars, false);
    std::function<mockturtle::aig_network::signal(uint32_t)> build =
        [&](uint32_t depth) -> mockturtle::aig_network::signal {
        if (depth == n_vars) return aig.get_constant(f(assignment));
        assignment[depth] = false;
        auto lo = build(depth + 1);
        assignment[depth] = true;
        auto hi = build(depth + 1);
        assignment[depth] = false;
        return aig_mux(pis[depth], hi, lo);
    };

    aig.create_po(n_vars == 0 ? aig.get_constant(f({})) : build(0));
    return aig;
}

namespace {

void write_dimacs(const CnfFormula& cnf, const std::string& path) {
    std::ofstream out(path);
    out << "p cnf " << cnf.num_vars << " " << cnf.clauses.size() << "\n";
    for (const auto& clause : cnf.clauses) {
        for (int lit : clause.literals) out << lit << " ";
        out << "0\n";
    }
}

// solver_binary распознаётся по имени: cadical/kissat оба следуют SAT
// Competition конвенции по exit code (10 SAT / 20 UNSAT) и по формату вывода
// "v <лит1> <лит2> ... 0" строк для witness — их и парсим, без специфики
// одного конкретного солвера.
SatResult run_solver(const std::string& solver_binary, const std::string& cnf_path,
                      uint32_t num_vars) {
    SatResult result;

    const std::string out_path = cnf_path + ".out";
    const std::string command = solver_binary + " \"" + cnf_path + "\" > \"" + out_path + "\" 2>&1";
    const int raw_status = std::system(command.c_str());
    if (raw_status == -1) return result;  // solver_ran остаётся false — команда не запустилась вовсе

#if BMM_HAVE_POSIX_WAIT
    const int exit_code = WIFEXITED(raw_status) ? WEXITSTATUS(raw_status) : -1;
#else
    const int exit_code = raw_status;
#endif

    if (exit_code != 10 && exit_code != 20) {
        // Ни SAT, ни UNSAT (солвер не найден в PATH -> shell вернёт 127, либо
        // UNKNOWN/таймаут) — сообщаем "не запустился" честно, а не гадаем.
        return result;
    }

    result.solver_ran = true;
    result.satisfiable = (exit_code == 10);

    if (result.satisfiable) {
        std::ifstream in(out_path);
        std::vector<bool> witness(num_vars + 1, false);  // 1-indexed, [0] не используется
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] != 'v') continue;
            std::istringstream iss(line.substr(1));
            int lit;
            while (iss >> lit) {
                if (lit == 0) break;
                const uint32_t var = static_cast<uint32_t>(lit < 0 ? -lit : lit);
                if (var <= num_vars) witness[var] = lit > 0;
            }
        }
        result.witness = witness;
    }

    std::remove(out_path.c_str());
    return result;
}

}  // namespace

SatResult solve_dimacs(const CnfFormula& cnf, const std::string& solver_binary) {
    // Уникальное имя во временной scratch-директории процесса — репозиторий не
    // предполагает параллельный запуск нескольких test_runner-ов в одном
    // рабочем каталоге, поэтому pid в имени файла достаточно для изоляции.
#if BMM_HAVE_POSIX_WAIT
    const long pid = static_cast<long>(::getpid());
#else
    const long pid = 0;
#endif
    const std::string cnf_path = "/tmp/bmm_sat_" + std::to_string(pid) + "_" +
                                  std::to_string(reinterpret_cast<uintptr_t>(&cnf)) + ".cnf";
    write_dimacs(cnf, cnf_path);
    auto result = run_solver(solver_binary, cnf_path, cnf.num_vars);
    std::remove(cnf_path.c_str());
    return result;
}

EquivalenceCheckResult check_aig_equivalence_via_sat(const mockturtle::aig_network& a,
                                                      const mockturtle::aig_network& b,
                                                      const std::string& solver_binary) {
    EquivalenceCheckResult result;

    const auto enc_a = encode_aig_tseitin(a);
    const auto enc_b = encode_aig_tseitin(b);
    if (enc_a.po_literals.size() != 1 || enc_b.po_literals.size() != 1) {
        return result;  // checked = false: контракт "один выход", см. core/CONVENTIONS.md п.4
    }

    // Собираем объединённую формулу: клозы A как есть, клозы B со сдвигом
    // переменных на num_vars(A), плюс Tseitin-XOR "miter = po_a XOR po_b",
    // плюс unit-клоза miter=true — SAT здесь означает "нашлась точка
    // расхождения", UNSAT — "функции эквивалентны".
    CnfFormula merged;
    merged.num_vars = enc_a.cnf.num_vars + enc_b.cnf.num_vars + 1;
    merged.clauses = enc_a.cnf.clauses;

    const int offset = static_cast<int>(enc_a.cnf.num_vars);
    auto shift = [&](int lit) { return lit > 0 ? lit + offset : lit - offset; };
    for (const auto& clause : enc_b.cnf.clauses) {
        CnfClause shifted;
        for (int lit : clause.literals) shifted.literals.push_back(shift(lit));
        merged.clauses.push_back(std::move(shifted));
    }

    // Связываем входы схем A и B: PI_a_i == PI_b_i. Без этого SAT-солвер
    // волен подставлять в A и B разные наборы входов, и формула становится
    // выполнимой почти всегда, даже для идентичных схем -- см. отчёт о
    // баге, подтверждено репродукцией на двух идентичных AIG.
    const uint32_t num_pis = a.num_pis();
    for (uint32_t i = 0; i < num_pis; ++i) {
        const int var_a = 2 + static_cast<int>(i);
        const int var_b = 2 + static_cast<int>(i) + offset;
        merged.clauses.push_back({{-var_a, var_b}});
        merged.clauses.push_back({{-var_b, var_a}});
    }

    const int po_a = enc_a.po_literals[0];
    const int po_b = shift(enc_b.po_literals[0]);
    const int miter = static_cast<int>(merged.num_vars);  // последняя переменная = miter
    // miter <-> (po_a XOR po_b), закодировано четырьмя клозами Tseitin-XOR,
    // плюс форсируем miter = true.
    merged.clauses.push_back({{-miter, po_a, po_b}});
    merged.clauses.push_back({{-miter, -po_a, -po_b}});
    merged.clauses.push_back({{miter, po_a, -po_b}});
    merged.clauses.push_back({{miter, -po_a, po_b}});
    merged.clauses.push_back({{miter}});

    const auto sat = solve_dimacs(merged, solver_binary);
    if (!sat.solver_ran) return result;  // checked = false — солвер недоступен, не FAIL

    result.checked = true;
    result.equivalent = !sat.satisfiable;

    if (sat.satisfiable && sat.witness) {
        // Восстанавливаем присвоение PI: в encode_aig_tseitin PI-переменные
        // идут сразу после var=1 (константа), в порядке foreach_pi — том же,
        // что и здесь.
        std::vector<bool> pi_values;
        int idx = 2;
        a.foreach_pi([&](auto) {
            pi_values.push_back(sat.witness->at(static_cast<size_t>(idx)));
            ++idx;
        });
        result.counterexample = pi_values;
    }

    return result;
}

}  // namespace bmm::verify
