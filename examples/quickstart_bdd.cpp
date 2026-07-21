// examples/quickstart_bdd.cpp — минимальный автономный пример использования
// bmm::Bdd/bmm::tt_to_bdd ВНЕ тестовой инфраструктуры (без Catch2, без
// verify/bmm_verify). См. examples/README.md за объяснением, почему здесь
// нужен весь этот lace_start/RUN/sylvan_init_bdd/sylvan_quit/lace_stop
// ритуал, а не просто "позвать tt_to_bdd() из main()".
//
// Сборка: этот файл — часть таргета `example_quickstart_bdd` (см.
// CMakeLists.txt). Запуск: ./build/example_quickstart_bdd

#include <cstdio>

#include <sylvan_obj.hpp>

#include <bmm/bdd/tt_to_bdd.hpp>
#include <bmm/core/common.hpp>

using namespace bmm;

namespace {

// Одна и та же маленькая тестовая функция, что и в examples/quickstart_non_bdd.cpp
// (majority на 3 переменных) — чтобы можно было сравнить два файла бок о бок
// и увидеть, что отличается ТОЛЬКО обвязка инициализации, а не сам вызов
// функции трансляции.
TruthTable make_majority3() {
    TruthTable tt(3);
    for (uint64_t idx = 0; idx < 8; ++idx) {
        if (__builtin_popcountll(idx) >= 2) kitty::set_bit(tt.raw(), idx);
    }
    return tt;
}

void print_bdd(const Bdd& bdd) {
    std::printf("majority3 как Bdd (n_vars=%u):\n", bdd.n_vars());
    for (uint64_t idx = 0; idx < 8; ++idx) {
        Assignment a(3);
        for (uint32_t b = 0; b < 3; ++b) a[b] = (idx >> b) & 1u;
        std::printf("  f(%u,%u,%u) = %d\n",
                     a[0] ? 1 : 0, a[1] ? 1 : 0, a[2] ? 1 : 0,
                     bdd.evaluate(a) ? 1 : 0);
    }
}

// Вся работа со sylvan::Bdd обязана идти внутри задачи, запущенной через
// Lace (см. examples/README.md) — поэтому тело примера оформлено как
// VOID_TASK_0, а не просто вызывается из main() напрямую.
VOID_TASK_0(run_example) {
    // Инициализация Sylvan: размеры таблиц узлов/кэша операций (нижняя/
    // верхняя граница по 2^22..2^26 записей) — те же значения, что и в
    // verify/test_main.cpp; для реальных нагрузок с большими BDD может
    // понадобиться больше, см. sylvan_obj.hpp / sylvan.h за смыслом
    // параметров sylvan_set_sizes().
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 22, 1LL << 26);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    TruthTable tt = make_majority3();
    Result<Bdd> result = tt_to_bdd(tt);

    if (!is_ok(result)) {
        std::printf("tt_to_bdd не удался: code=%d, %s\n",
                     static_cast<int>(error(result).code), error(result).message.c_str());
    } else {
        print_bdd(value(result));
    }

    sylvan::sylvan_quit();
}

}  // namespace

int main() {
    // deque_size = 1<<21 — НЕ дефолт Lace (дефолт заметно меньше). Это
    // эмпирически подобранное значение из этой же кодовой базы (см.
    // verify/test_main.cpp и examples/README.md): меньшие значения
    // (проверено — 1<<18) на реальных плотных нагрузках приводят к
    // "Lace fatal error: Task stack overflow! Aborting" — падению без
    // единого шанса поймать это как Result<T>/исключение, потому что это
    // фатальная ошибка самого Lace, а не C++ exception. На этом маленьком
    // примере (n=3) разницы не будет видно вообще, но при переходе на
    // реальные размеры входа — будет, и тогда искать источник этого краха
    // с нуля обойдётся дорого. Держите это значение, а не дефолт Lace.
    const int n_workers = 0;  // 0 = автоопределение по числу ядер
    const size_t deque_size = 1ULL << 21;

    lace_start(n_workers, deque_size);
    RUN(run_example);
    lace_stop();

    return 0;
}
