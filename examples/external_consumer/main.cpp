// examples/external_consumer/main.cpp — вызывается ТОЛЬКО из
// examples/external_consumer/CMakeLists.txt (отдельный CMake-проект, см.
// комментарий там за тем, что именно это доказывает). Намеренно другая
// тестовая функция, чем examples/quickstart_non_bdd.cpp (AND3 вместо
// majority3) — чтобы вывод нельзя было спутать с тем, что просто собрался
// старый бинарник из основного дерева.

#include <cstdio>

#include <bmm/aig/tt_to_aig.hpp>
#include <bmm/core/common.hpp>

using namespace bmm;

namespace {

TruthTable make_and3() {
    TruthTable tt(3);
    kitty::set_bit(tt.raw(), 7);  // f(1,1,1) = 1, всё остальное — 0
    return tt;
}

}  // namespace

int main() {
    TruthTable tt = make_and3();
    Result<Aig> result = tt_to_aig(tt);

    if (!is_ok(result)) {
        std::printf("[external_consumer] tt_to_aig не удался: code=%d, %s\n",
                     static_cast<int>(error(result).code), error(result).message.c_str());
        return 1;
    }

    const Aig& aig = value(result);
    bool all_correct = true;
    for (uint64_t idx = 0; idx < 8; ++idx) {
        Assignment a(3);
        for (uint32_t b = 0; b < 3; ++b) a[b] = (idx >> b) & 1u;
        const bool expected = (idx == 7);
        const bool actual = aig.evaluate(a);
        if (actual != expected) all_correct = false;
    }

    std::printf("[external_consumer] find_package(bmm-translib) + tt_to_aig: %s\n",
                 all_correct ? "OK (AND3 корректно построен и оценён)" : "FAIL (расхождение)");
    return all_correct ? 0 : 1;
}
