// Временный диагностический бинарь — ТОЛЬКО под внешним `timeout` (Lace
// abort() не ловится изнутри процесса, см. anf/README.md §2). Проверяет,
// ведёт ли себя anf_to_bdd (даже с FORCE-эвристикой) катастрофически на
// реальном плотном ANF n=100/M=10000 (persons.iis.nsk.su) — заведомо худший
// класс входа для BDD, чем что-либо использованное в этой сессии раньше
// (bent_mm_n40 — разреженный, n/2=20 мономов, но криптографически "плохой"
// для BDD по другой причине; здесь — n=100, M=10000, средняя длина монома
// ~65 из 100 — граф взаимодействия переменных близок к ПОЛНОМУ K_100, у
// FORCE физически нет структуры, которую можно было бы эксплуатировать).

#include "anf/anf_to_bdd.hpp"
#include "benchmarks/anf_dimacs_loader.hpp"

#include <sylvan_obj.hpp>

#include <chrono>
#include <cstdio>

using namespace bmm;
using namespace bmm::benchmarks;

VOID_TASK_0(run_diag) {
    sylvan::sylvan_set_sizes(1LL << 20, 1LL << 24, 1LL << 16, 1LL << 20);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    auto anf = load_anf_dimacs("benchmarks/data/iis-nsk/100-10k-rnd/10-100x90-100.01.anf");
    if (!anf) {
        std::printf("FAIL: не удалось загрузить\n");
        std::fflush(stdout);
        sylvan::sylvan_quit();
        return;
    }
    std::printf("загружено: n_vars=%u\n", anf->n_vars());
    std::fflush(stdout);

    std::printf("anf_to_bdd (Force, умолчание) — запуск...\n");
    std::fflush(stdout);
    auto t0 = std::chrono::steady_clock::now();
    auto r = anf_to_bdd(*anf);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (is_ok(r)) {
        std::printf("PASS: %.3f мс, NodeCount=%zu\n", ms,
                    static_cast<size_t>(value(r).raw().NodeCount()));
    } else {
        std::printf("FAIL: %s (%.3f мс)\n", error(r).message.c_str(), ms);
    }
    std::fflush(stdout);

    sylvan::sylvan_quit();
}

int main() {
    // ИСПРАВЛЕНИЕ найденного в этой сессии краха ("Lace fatal error: Task
    // stack overflow! Aborting"): дело НЕ в нативном C-стеке воркера
    // (lace_set_stacksize() — проверено эмпирически, 256 MiB стека крах НЕ
    // убирает), а в очереди SPAWN-задач Lace — второй параметр lace_start
    // (dqsize). Дефолт (0) не хватает для объёма рекурсивных Ite()-вызовов
    // Sylvan на этом датасете; 1<<18 — всё ещё крах, 1<<21 (2М
    // записей/воркер) — крах не воспроизводится (проверено эмпирически).
    // См. verify/test_main.cpp за тем же исправлением, применённым проектно.
    lace_start(0, 1ULL << 21);
    RUN(run_diag);
    lace_stop();
    return 0;
}
