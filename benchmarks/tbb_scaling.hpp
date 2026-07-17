// benchmarks/tbb_scaling.hpp — общий харнесс для обязательного бенчмарка
// "однопоточная vs TBB-версия" у функций трансляции, использующих TBB
// (см. core/CONVENTIONS.md п.6: aig_to_*, anf_to_*, tt_to_aig, tt_to_anf).
//
// Ключевая идея: не просим студента писать ОТДЕЛЬНУЮ однопоточную копию
// алгоритма — это было бы дублированием кода и источником рассинхронизации
// (однопоточная "версия для сравнения" тихо перестаёт совпадать с реальной
// после правки основной). Вместо этого замеряем ОДИН И ТОТ ЖЕ вызов дважды:
// один раз под tbb::global_control, ограничивающим TBB одним потоком
// (официальный, документированный способ Intel/oneTBB получить
// однопоточное поведение существующего task_group-кода без переписывания
// его логики), и один раз без ограничения (arena на все ядра). Разница —
// это честный эффект параллелизма именно вашей реализации, а не
// гипотетической "второй версии".
//
// Формат результата (ScalingPoint) и печать — общие с OpenMP-версией этого
// же харнесса, см. benchmarks/scaling.hpp и benchmarks/openmp_scaling.hpp:
// строки BMM_BENCH/BMM_BENCH_SUMMARY в выводе теста одинаковы независимо от
// backend'а конкретной функции.
//
// Использование (см. секции "*_tbb_scaling" в test_aig.cpp/test_anf.cpp):
//   auto point = bmm::benchmarks::measure_scaling("n=12", [&] {
//       auto result = tt_to_aig(input);
//       benchmark::doNotOptimizeAway(result);
//   });
//   bmm::benchmarks::print_bench_line("tt_to_aig", point);

#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <oneapi/tbb/global_control.h>

#include "benchmarks/scaling.hpp"

namespace bmm::benchmarks {

// Прогревает `work` один раз (первый вызов часто содержит одноразовые
// издержки — аллокация TBB-арены, ленивая инициализация статиков), затем
// замеряет под 1-поточным global_control и под дефолтным (все ядра),
// возвращает обе точки и их отношение.
inline ScalingPoint measure_scaling(const std::string& size_label,
                                     const std::function<void()>& work) {
    work();  // прогрев — не входит в измерение

    ScalingPoint point;
    point.size_label = size_label;

    {
        oneapi::tbb::global_control limit(oneapi::tbb::global_control::max_allowed_parallelism, 1);
        const auto start = std::chrono::steady_clock::now();
        work();
        const auto end = std::chrono::steady_clock::now();
        point.single_threaded_ms = std::chrono::duration<double, std::milli>(end - start).count();
    }
    {
        // Без ограничения — oneTBB сам выбирает арену по числу ядер контейнера.
        const auto start = std::chrono::steady_clock::now();
        work();
        const auto end = std::chrono::steady_clock::now();
        point.parallel_ms = std::chrono::duration<double, std::milli>(end - start).count();
    }

    point.speedup = point.parallel_ms > 0.0 ? point.single_threaded_ms / point.parallel_ms : 0.0;
    return point;
}

}  // namespace bmm::benchmarks
