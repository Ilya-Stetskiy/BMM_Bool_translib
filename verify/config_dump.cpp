// verify/config_dump.cpp — крошечная утилита: печатает, какие условные
// ветки core/common.hpp активны в текущей сборке. Единственный источник
// истины для generate_status.sh (не дублируем детектирование фич на уровне
// bash/CMake — препроцессор common.hpp уже это знает).

#include <cstdio>

#include "core/common.hpp"

int main() {
    std::printf("BMM_INFO result_backend %s\n",
                BMM_HAS_STD_EXPECTED ? "std::expected(C++23)" : "std::variant(fallback)");
    std::printf("BMM_INFO anf_backend %s\n",
                BMM_HAVE_BRIAL ? "BRiAl(BoolePolynomial)" : "AnfFallback(monomial-set)");
    return 0;
}
