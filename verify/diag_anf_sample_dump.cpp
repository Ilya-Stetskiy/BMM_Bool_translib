// Временный диагностический бинарь — генерирует случайные точки + значения
// Anf::evaluate() для конкретного .anf-файла и пишет их в файл. Отдельный
// процесс (verify/diag_dddmp_sweep.cpp) читает эти точки и сверяет со
// встроенным DDDMP-эталоном той же схемы — РАЗДЕЛЕНЫ на два бинаря
// намеренно: benchmarks/anf_dimacs_loader.hpp тянет core/anf_repr.hpp
// (BRiAl, bundled polybori/cudd/cudd.h), benchmarks/dddmp_loader.hpp тянет
// системный /usr/local/include/cudd.h — оба в одной единице трансляции
// конфликтуют (конфликтующие объявления одних и тех же имён функций CUDD
// с разными типами менеджера, см. подробное объяснение в шапке
// benchmarks/dddmp_loader.hpp) — тот же приём, что и обмен witness между
// solve_dimacs и построенным AIG в verify/diag_cnf_dataset.cpp, только
// через промежуточный файл вместо прямого вызова в одном процессе.

#include "benchmarks/anf_dimacs_loader.hpp"

#include <cstdio>
#include <fstream>
#include <random>
#include <string>

using namespace bmm;
using namespace bmm::benchmarks;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <anf_path> <out_path> [n_samples=30]\n", argv[0]);
        return 1;
    }
    std::string anf_path = argv[1];
    std::string out_path = argv[2];
    int n_samples = argc > 3 ? std::atoi(argv[3]) : 30;

    auto anf = load_anf_dimacs(anf_path);
    if (!anf) {
        std::printf("FAIL: не удалось загрузить %s\n", anf_path.c_str());
        return 1;
    }

    std::ofstream out(out_path);
    out << anf->n_vars() << "\n";

    std::mt19937_64 rng(20260719);
    std::uniform_int_distribution<int> bit(0, 1);
    Assignment assignment(anf->n_vars(), false);
    for (int s = 0; s < n_samples; ++s) {
        for (uint32_t i = 0; i < anf->n_vars(); ++i) {
            assignment[i] = bit(rng) != 0;
            out << (assignment[i] ? '1' : '0');
        }
        bool value = anf->evaluate(assignment);
        out << " " << (value ? 1 : 0) << "\n";
    }
    std::printf("PASS: n_vars=%u, %d сэмплов записано в %s\n", anf->n_vars(), n_samples,
                out_path.c_str());
    return 0;
}
