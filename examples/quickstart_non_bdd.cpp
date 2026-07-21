// examples/quickstart_non_bdd.cpp — минимальный автономный пример
// использования bmm::Aig/bmm::tt_to_aig ВНЕ тестовой инфраструктуры.
//
// Сравните этот файл с examples/quickstart_bdd.cpp: та же самая тестовая
// функция (majority на 3 переменных), тот же паттерн (Result<T> -> is_ok/
// value/error), но БЕЗ единой строчки про Lace/Sylvan — main() вызывает
// tt_to_aig() напрямую. Это не сокращённый пример, а показ того, что
// требование "инициализируйте Sylvan/Lace перед использованием" относится
// ИСКЛЮЧИТЕЛЬНО к представлению Bdd (core/CONVENTIONS.md п.6, правило 1) —
// остальные четыре представления (Aig, Anf, Thr, TruthTable) не требуют
// никакой глобальной инициализации вообще, ими можно пользоваться сразу
// из main() любого приложения, слинковавшего только нужный bmm_<format>.
//
// Сборка: таргет `example_quickstart_non_bdd` (см. CMakeLists.txt).
// Запуск: ./build/example_quickstart_non_bdd

#include <cstdio>

#include "aig/tt_to_aig.hpp"
#include "core/common.hpp"

using namespace bmm;

namespace {

TruthTable make_majority3() {
    TruthTable tt(3);
    for (uint64_t idx = 0; idx < 8; ++idx) {
        if (__builtin_popcountll(idx) >= 2) kitty::set_bit(tt.raw(), idx);
    }
    return tt;
}

}  // namespace

int main() {
    TruthTable tt = make_majority3();
    Result<Aig> result = tt_to_aig(tt);

    if (!is_ok(result)) {
        std::printf("tt_to_aig не удался: code=%d, %s\n",
                     static_cast<int>(error(result).code), error(result).message.c_str());
        return 1;
    }

    const Aig& aig = value(result);
    std::printf("majority3 как Aig (n_vars=%u):\n", aig.n_vars());
    for (uint64_t idx = 0; idx < 8; ++idx) {
        Assignment a(3);
        for (uint32_t b = 0; b < 3; ++b) a[b] = (idx >> b) & 1u;
        std::printf("  f(%u,%u,%u) = %d\n",
                     a[0] ? 1 : 0, a[1] ? 1 : 0, a[2] ? 1 : 0,
                     aig.evaluate(a) ? 1 : 0);
    }
    return 0;
}
