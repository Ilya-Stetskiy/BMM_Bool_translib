// fuzz/fuzz_anf_dimacs_loader.cpp — libFuzzer-харнесс для
// benchmarks/anf_dimacs_loader.hpp::load_anf_dimacs. См.
// fuzz/fuzz_cnf_dimacs_loader.cpp за полным обоснованием подхода (тот же
// паттерн: временный файл на итерацию, фаззится только парсинг).
//
// ВАЖНО: load_anf_dimacs собирает Anf через BoolePolynomial, когда
// BMM_HAVE_BRIAL=1 (см. core/anf_repr.hpp) — BRiAl подтверждённо не
// потокобезопасен (ThreadSanitizer, aig/README.md §1.3), но libFuzzer в
// однопроцессном режиме (без -jobs=N>1) не создаёт параллелизма внутри
// процесса, поэтому это не входит в противоречие с тем ограничением здесь.

#include "benchmarks/anf_dimacs_loader.hpp"

#include <cstdint>
#include <fstream>
#include <string>

#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const std::string path = "/tmp/bmm_fuzz_anf_" + std::to_string(getpid()) + ".anf";

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    auto result = bmm::benchmarks::load_anf_dimacs(path);
    (void)result;

    return 0;
}
