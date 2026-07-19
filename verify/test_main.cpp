// verify/test_main.cpp — заменяет автогенерируемый main() от Catch2 для
// всех test_aig/test_bdd/test_anf/test_thr. Sylvan/Lace требует, чтобы любой
// код, работающий с sylvan::Bdd, выполнялся внутри Lace-задачи (вызванной
// через RUN/CALL) — вызов Bdd-операций напрямую из потока, который лишь
// позвал lace_start(), падает в SIGSEGV внутри Sylvan (llmsset_lookup),
// потому что этот поток не зарегистрирован как Lace-воркер. Поэтому вся
// сессия Catch2 запускается одной верхнеуровневой Lace-задачей — по образцу
// examples/simple.cpp из самого Sylvan (lace_start -> RUN(_main) ->
// sylvan_init_* -> CALL(work)).
#include <catch2/catch_session.hpp>

#include <sylvan_obj.hpp>

namespace {

int g_argc = 0;
char** g_argv = nullptr;
int g_catch_result = 0;

VOID_TASK_0(bmm_run_tests) {
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 22, 1LL << 26);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    g_catch_result = Catch::Session().run(g_argc, g_argv);

    sylvan::sylvan_quit();
}

}  // namespace

int main(int argc, char* argv[]) {
    g_argc = argc;
    g_argv = argv;

    const int n_workers = 0;  // автоопределение
    // deque_size=0 (дефолт Lace) на реальных данных этой сессии (ANF,
    // n=100, 10000 плотных мономов, persons.iis.nsk.su) приводил к
    // НЕПОЙМАННОМУ краху "Lace fatal error: Task stack overflow! Aborting"
    // при построении BDD (anf_to_bdd) — не хватало очереди SPAWN-задач для
    // объёма рекурсивных Ite()-вызовов Sylvan, а не нативного C-стека
    // (lace_set_stacksize() эту проблему НЕ решает, проверено эмпирически).
    // 1<<21 (2М записей/воркер) эмпирически проверено: краха не возникает
    // (1<<18 — всё ещё крах, 1<<21 — нет); тот же запас поставлен во всех
    // местах, где вызывается lace_start (chain_tests.cpp,
    // real_datasets_tests.cpp, anf/thr bench_real_corpus.cpp).
    const size_t deque_size = 1ULL << 21;
    lace_start(n_workers, deque_size);
    RUN(bmm_run_tests);
    lace_stop();

    return g_catch_result;
}