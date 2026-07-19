// Временный диагностический бинарь — прогоняет AIG, построенные cnf_to_aig
// (benchmarks/cnf_dimacs_loader.hpp) из РЕАЛЬНЫХ SATLIB/DIMACS-инстансов,
// через остальные переводы (aig_to_bdd/aig_to_anf/aig_to_thr/aig_to_tt) —
// то, ради чего вообще нужен CNF-импортёр (открыть 50К+ реальных SAT-
// инстансов для нагрузки библиотеки, не только для самопроверки cnf_to_aig
// против солвера — см. diag_cnf_dataset.cpp). Aig_to_bdd/aig_to_anf уже
// защищены (FORCE-эвристика/дедлайн по времени, см. SESSION_REPORT.md) —
// ожидаем либо PASS, либо честный контролируемый отказ, не зависание.

#include "benchmarks/cnf_dimacs_loader.hpp"
#include "verify/chain_utils.hpp"

#include <sylvan_obj.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace bmm;
using namespace bmm::benchmarks;
using namespace bmm::chains;

namespace {

void try_target(Report& rep, const std::string& name, const AnyRepr& aig_repr, Repr to) {
    auto t0 = std::chrono::steady_clock::now();
    auto result = translate_step(Repr::Aig, to, aig_repr);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("  [построение Aig->%s заняло %.3f мс]\n", repr_name(to), ms);
    std::fflush(stdout);

    if (!is_ok(result)) {
        rep.line("- SKIP " + name + ": Aig->" + repr_name(to) + " (" + error(result).message +
                  ", " + std::to_string(ms) + " мс)");
        return;
    }
    auto t2 = std::chrono::steady_clock::now();
    bool eq = reprs_equivalent_sampled(aig_repr, value(result), 500, 20260719);
    auto t3 = std::chrono::steady_clock::now();
    double eq_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::printf("  [проверка эквивалентности заняла %.3f мс]\n", eq_ms);
    std::fflush(stdout);
    rep.bullet(eq, name + ": Aig->" + repr_name(to) + " (построение " + std::to_string(ms) +
                       " мс, проверка " + std::to_string(eq_ms) + " мс)");
}

void check_one(Report& rep, const std::string& path, bool try_bdd) {
    auto cnf = load_cnf_dimacs(path);
    if (!cnf) {
        rep.line("- FAIL " + path + ": не удалось распарсить");
        return;
    }
    Aig aig = cnf_to_aig(*cnf);
    rep.line("== " + path + " (n=" + std::to_string(aig.n_vars()) +
             ", гейтов=" + std::to_string(aig.raw().num_gates()) + ") ==");

    AnyRepr aig_repr = AnyRepr(aig);
    if (try_bdd) {
        try_target(rep, path, aig_repr, Repr::Bdd);
    } else {
        rep.line("- SKIP " + path +
                 ": Aig->Bdd (известно медленно/не завершается на этом входе — см. "
                 "SESSION_REPORT.md, проверено отдельно с внешним timeout)");
    }
    try_target(rep, path, aig_repr, Repr::Anf);
    try_target(rep, path, aig_repr, Repr::Thr);
    try_target(rep, path, aig_repr, Repr::Tt);
}

int g_result = 0;

VOID_TASK_0(main_task) {
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 18, 1LL << 22);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    Report rep;
    check_one(rep, "benchmarks/data/satlib/RND3SAT/uf20-01.cnf", /*try_bdd=*/true);   // n=20
    check_one(rep, "benchmarks/data/satlib/PLANNING/BlocksWorld/anomaly.cnf",
              /*try_bdd=*/true);                                                      // n=48
    check_one(rep, "benchmarks/data/dimacs-cnf/hole6.cnf", /*try_bdd=*/true);          // n=42, UNSAT
    check_one(rep, "benchmarks/data/dimacs-cnf/aim-100-1_6-yes1-1.cnf",
              /*try_bdd=*/false);  // n=100 — Aig->Bdd не завершается за разумное время

    g_result = rep.failed_checks == 0 ? 0 : 1;
    sylvan::sylvan_quit();
}

}  // namespace

int main() {
    const size_t deque_size = 1ULL << 21;  // см. verify/test_main.cpp
    lace_start(0, deque_size);
    RUN(main_task);
    lace_stop();
    return g_result;
}
