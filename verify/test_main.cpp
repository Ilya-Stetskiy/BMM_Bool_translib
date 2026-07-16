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

    const int n_workers = 0;      // автоопределение
    const size_t deque_size = 0;  // размер очереди задач Lace по умолчанию
    lace_start(n_workers, deque_size);
    RUN(bmm_run_tests);
    lace_stop();

    return g_catch_result;
}