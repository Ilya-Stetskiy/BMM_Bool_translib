// fuzz/fuzz_cnf_dimacs_loader.cpp — libFuzzer-харнесс для
// benchmarks/cnf_dimacs_loader.hpp::load_cnf_dimacs — единственного
// собственного парсера untrusted-формата в этом загрузчике (в отличие от
// benchmarks/dddmp_loader.hpp, который в основном проксирует парсер CUDD
// — фаззить его означало бы фаззить CUDD, не код проекта).
//
// Фаззит именно парсинг (load_cnf_dimacs), не cnf_to_aig — находка, ради
// которой заведён этот харнесс (см. kMaxReasonableCnfVars в самом
// cnf_dimacs_loader.hpp), была в заголовке файла ДО какого-либо перебора
// строк, то есть внутри load_cnf_dimacs.
//
// load_cnf_dimacs принимает путь к файлу, не буфер в памяти — пишем вход
// фаззера во временный файл на каждой итерации (стандартный паттерн для
// фаззинга файловых парсеров через libFuzzer; дороже по I/O за итерацию,
// чем буфер в памяти, но не требует менять публичный API загрузчика ради
// тестируемости — тот же путь, что реально вызывают test_real_datasets/
// diag_cnf_dataset).
//
// Сборка: -DBMM_BUILD_FUZZERS=ON, требует Clang (-fsanitize=fuzzer — флаг
// только Clang, GCC его не поддерживает) — см. fuzz/README.md.

#include "benchmarks/cnf_dimacs_loader.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // PID в имени — на случай запуска с -jobs=N>1 (несколько процессов).
    // Внутри одного процесса libFuzzer вызывает эту функцию строго
    // последовательно, поэтому статический путь безопасен.
    static const std::string path = "/tmp/bmm_fuzz_cnf_" + std::to_string(getpid()) + ".cnf";

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    auto result = bmm::benchmarks::load_cnf_dimacs(path);
    (void)result;  // интересует падение/OOM/UB (ASan/UBSan), не сам результат парсинга

    return 0;
}
