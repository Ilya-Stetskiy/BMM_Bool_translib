// benchmarks/openmp_scaling.hpp — аналог benchmarks/tbb_scaling.hpp для
// функций, использующих OpenMP (core/CONVENTIONS.md п.6: thr/*, кроме
// thr_to_bdd — тот идёт по правилу "Bdd — только Sylvan/Lace" с более
// высоким приоритетом; и *_to_tt/tt_to_* сверх уже покрытых TBB-правилом
// aig_to_*/anf_to_*/tt_to_aig/tt_to_anf).
//
// В отличие от TBB (RAII-скоуп tbb::global_control, ограничивающий именно
// вызов внутри блока), OpenMP управляется ГЛОБАЛЬНЫМ изменяемым состоянием
// (omp_set_num_threads() — влияет на все последующие "#pragma omp parallel"
// в процессе, не только на один вызов). Поэтому здесь нет RAII-объекта:
// явно выставляем 1 поток перед однопоточным замером и явно возвращаем
// дефолтное число потоков перед параллельным. Само измерение (ScalingPoint,
// печать) идентично TBB-версии — оба используют один и тот же
// benchmarks/scaling.hpp, поэтому вывод (BMM_BENCH/BMM_BENCH_SUMMARY)
// выглядит одинаково независимо от backend'а конкретной функции.
//
// Использование (см. секции "*_openmp_scaling" в test_thr.cpp):
//   auto point = bmm::benchmarks::measure_scaling_omp("n=12", [&] {
//       auto result = tt_to_thr(input);
//       benchmark::doNotOptimizeAway(result);
//   });
//   bmm::benchmarks::print_bench_line("tt_to_thr", point);

#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <omp.h>

#include "benchmarks/scaling.hpp"

namespace bmm::benchmarks {

inline ScalingPoint measure_scaling_omp(const std::string& size_label,
                                         const std::function<void()>& work) {
    work();  // прогрев — не входит в измерение

    ScalingPoint point;
    point.size_label = size_label;

    const int default_threads = omp_get_max_threads();

    omp_set_num_threads(1);
    {
        const auto start = std::chrono::steady_clock::now();
        work();
        const auto end = std::chrono::steady_clock::now();
        point.single_threaded_ms = std::chrono::duration<double, std::milli>(end - start).count();
    }

    omp_set_num_threads(default_threads);
    {
        // Дефолтное число потоков — omp_get_max_threads() на старте процесса
        // (обычно = число логических ядер контейнера, если OMP_NUM_THREADS
        // не выставлен явно).
        const auto start = std::chrono::steady_clock::now();
        work();
        const auto end = std::chrono::steady_clock::now();
        point.parallel_ms = std::chrono::duration<double, std::milli>(end - start).count();
    }

    point.speedup = point.parallel_ms > 0.0 ? point.single_threaded_ms / point.parallel_ms : 0.0;
    return point;
}

}  // namespace bmm::benchmarks
