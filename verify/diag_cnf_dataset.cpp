// Временный диагностический бинарь — проверить benchmarks/
// cnf_dimacs_loader.hpp (cnf_to_aig) на реальном корпусе DIMACS CNF
// (benchmarks/data/dimacs-cnf/*.cnf) независимо от самой библиотеки:
// используем внешний SAT-солвер (CaDiCaL, через уже существующий
// verify/sat_encoding::solve_dimacs) ДВАЖДЫ —
//   1) на исходной CNF-формуле напрямую (эталон: выполнима/нет, и witness
//      если выполнима);
//   2) на Tseitin-кодировании ПОСТРОЕННОГО нами AIG (encode_aig_tseitin из
//      того же verify/sat_encoding) с принудительным po=true — если наш
//      cnf_to_aig корректен, "AIG выполним" должно СОВПАДАТЬ с "исходная
//      CNF выполнима" (обе стороны выражают одну и ту же выполнимость), а
//      witness исходной CNF должен давать aig.evaluate(witness) == true.
// Ни разу не полагаемся на сам cnf_to_aig/mockturtle для получения
// "правильного ответа" — это два независимых способа получить один и тот
// же факт (сам SAT-solver и построенный по нему индикатор), см.
// core/CONVENTIONS.md п.7 про независимость верификации.
//
// Два режима: без аргументов — подробный прогон на 5 отобранных вручную
// файлах (разные n, известный SAT/UNSAT по имени файла); с аргументом
// "sweep" — тихий прогон по ВСЕМ *.cnf в benchmarks/data/dimacs-cnf (241
// файл), только итоговая сводка (не флудить построчно на 241 файл).

#include "benchmarks/cnf_dimacs_loader.hpp"
#include "verify/sat_encoding/sat_encoding.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace bmm;
using namespace bmm::benchmarks;
using namespace bmm::verify;

namespace {

enum class Outcome { Pass, Fail, SkipNoSolver };

Outcome check_one(const std::string& path, bool verbose) {
    auto print = [&](const char* fmt, auto&&... args) {
        if (verbose) {
            std::printf(fmt, args...);
            std::fflush(stdout);
        }
    };

    print("== %s ==\n", path.c_str());

    auto cnf = load_cnf_dimacs(path);
    if (!cnf) {
        std::printf("FAIL %s: не удалось распарсить\n", path.c_str());
        std::fflush(stdout);
        return Outcome::Fail;
    }
    print("PASS парсинг: n_vars=%u, n_clauses=%zu\n", cnf->num_vars, cnf->clauses.size());

    Aig aig = cnf_to_aig(*cnf);
    print("PASS cnf_to_aig: n_gates=%zu, n_vars(aig)=%u\n",
          static_cast<size_t>(aig.raw().num_gates()), aig.n_vars());

    auto ref = solve_dimacs(*cnf, "cadical");
    if (!ref.solver_ran) {
        print("SKIP: cadical не запустился\n");
        return Outcome::SkipNoSolver;
    }
    print("  эталон (cadical): %s\n", ref.satisfiable ? "SAT" : "UNSAT");

    auto enc = encode_aig_tseitin(aig.raw());
    if (enc.po_literals.size() != 1) {
        std::printf("FAIL %s: encode_aig_tseitin вернул не 1 PO\n", path.c_str());
        std::fflush(stdout);
        return Outcome::Fail;
    }
    CnfFormula forced = enc.cnf;
    forced.clauses.push_back({{enc.po_literals[0]}});
    auto aig_sat = solve_dimacs(forced, "cadical");
    if (!aig_sat.solver_ran) {
        print("SKIP: cadical не запустился на Tseitin(AIG)\n");
        return Outcome::SkipNoSolver;
    }

    if (aig_sat.satisfiable != ref.satisfiable) {
        std::printf("FAIL %s: Tseitin(AIG)+po=true дал %s, эталон %s\n", path.c_str(),
                     aig_sat.satisfiable ? "SAT" : "UNSAT", ref.satisfiable ? "SAT" : "UNSAT");
        std::fflush(stdout);
        return Outcome::Fail;
    }
    print("PASS независимая проверка через Tseitin(AIG)+po=true: %s\n",
          aig_sat.satisfiable ? "SAT" : "UNSAT");

    if (ref.satisfiable && ref.witness) {
        Assignment assignment(cnf->num_vars, false);
        for (uint32_t v = 1; v <= cnf->num_vars; ++v) assignment[v - 1] = ref.witness->at(v);
        if (!aig.evaluate(assignment)) {
            std::printf("FAIL %s: aig.evaluate(witness) != true\n", path.c_str());
            std::fflush(stdout);
            return Outcome::Fail;
        }
        print("PASS aig.evaluate(witness исходной CNF) == true\n");
    }

    print("%s", "\n");
    return Outcome::Pass;
}

}  // namespace

int main(int argc, char** argv) {
    bool sweep_mode = argc > 1 && std::string(argv[1]) == "sweep";

    if (!sweep_mode) {
        std::vector<std::string> files = {
            "benchmarks/data/dimacs-cnf/hole6.cnf",              // pigeonhole, UNSAT
            "benchmarks/data/dimacs-cnf/aim-50-1_6-no-1.cnf",    // AIM, "-no-" = UNSAT
            "benchmarks/data/dimacs-cnf/aim-50-1_6-yes1-1.cnf",  // AIM, "-yes1-" = SAT
            "benchmarks/data/dimacs-cnf/aim-100-1_6-no-1.cnf",
            "benchmarks/data/dimacs-cnf/aim-100-1_6-yes1-1.cnf",
        };
        int failures = 0;
        for (const auto& f : files) {
            if (check_one(f, /*verbose=*/true) == Outcome::Fail) ++failures;
        }
        std::printf("=== ИТОГ: %zu файлов, %d провалов ===\n", files.size(), failures);
        return failures == 0 ? 0 : 1;
    }

    int total = 0, passed = 0, failed = 0, skipped = 0;
    for (const auto& entry : std::filesystem::directory_iterator("benchmarks/data/dimacs-cnf")) {
        if (entry.path().extension() != ".cnf") continue;
        ++total;
        switch (check_one(entry.path().string(), /*verbose=*/false)) {
            case Outcome::Pass: ++passed; break;
            case Outcome::Fail: ++failed; break;
            case Outcome::SkipNoSolver: ++skipped; break;
        }
        if (total % 20 == 0) {
            std::printf("... %d файлов обработано (pass=%d fail=%d skip=%d)\n", total, passed,
                        failed, skipped);
            std::fflush(stdout);
        }
    }
    std::printf("=== ИТОГ SWEEP: %d файлов, %d PASS, %d FAIL, %d SKIP ===\n", total, passed, failed,
                skipped);
    return failed == 0 ? 0 : 1;
}
