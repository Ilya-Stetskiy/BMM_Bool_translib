// Временный диагностический бинарь — проверить API Dddmp_cuddBddArrayLoad
// (libcudd.a, встроенный dddmp) на реальном .bdd файле ПЕРЕД тем, как
// строить полный мост CUDD DdNode* -> sylvan::Bdd. Только CUDD/dddmp, без
// Sylvan и БЕЗ core/anf_repr.hpp (BRiAl) в этом же TU — избегаем
// потенциального конфликта cudd.h (BRiAl тянет свой bundled cudd.h, см.
// core/anf_repr.hpp п.о "сознательно отделён").

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include <cudd.h>
#include <dddmp.h>
}

int main(int argc, char** argv) {
    std::string path = argc > 1
                            ? argv[1]
                            : "benchmarks/data/iis-nsk/bdd-bench/Cookbook__adder_tree_node.blif.bdd";

    DdManager* mgr = Cudd_Init(0, 0, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    if (!mgr) {
        std::printf("FAIL: Cudd_Init\n");
        return 1;
    }

    DdNode** roots = nullptr;
    int n_roots = Dddmp_cuddBddArrayLoad(mgr, DDDMP_ROOT_MATCHLIST, nullptr, DDDMP_VAR_MATCHIDS,
                                          nullptr, nullptr, nullptr, DDDMP_MODE_DEFAULT,
                                          const_cast<char*>(path.c_str()), nullptr, &roots);

    if (n_roots <= 0 || !roots) {
        std::printf("FAIL: Dddmp_cuddBddArrayLoad вернул n_roots=%d\n", n_roots);
        return 1;
    }
    std::printf("PASS: n_roots=%d\n", n_roots);
    std::printf("Cudd_ReadSize(mgr) = %d (число переменных менеджера)\n", Cudd_ReadSize(mgr));

    for (int i = 0; i < n_roots && i < 3; ++i) {
        DdNode* r = roots[i];
        std::printf("root[%d]: DagSize=%d, complement=%d\n", i, Cudd_DagSize(r),
                    Cudd_IsComplement(r) ? 1 : 0);
    }

    for (int i = 0; i < n_roots; ++i) Cudd_RecursiveDeref(mgr, roots[i]);
    free(roots);
    Cudd_Quit(mgr);
    return 0;
}
