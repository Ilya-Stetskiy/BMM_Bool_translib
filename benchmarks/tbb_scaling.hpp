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

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <oneapi/tbb/global_control.h>

#include "benchmarks/scaling.hpp"

namespace bmm::benchmarks {

namespace detail {
inline double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
}  // namespace detail

// ИСПРАВЛЕНО (см. aig/README.md §1.3): раньше это был ОДИН замер на
// каждую сторону (1-поточную и параллельную) после одного прогрева — на
// суб-миллисекундных функциях (типичные размеры тестового набора) единичный
// замер — это в основном шум OS-планировщика, а не сигнал. Конкретный
// задокументированный провал: холодный статический кэш (`get_ring()` в
// aig_to_anf.cpp) давал ложные "ускорения" 20x-90x в отдельном
// самодельном бенчмарке этой же сессии — здесь тот же риск в меньшем
// масштабе (без warmup-бага, но с той же чувствительностью к единичному
// замеру). Методология исправлена на общую для всего проекта (см.
// anf/README.md/aig/README.md): 1 прогрев (не входит в измерение) + медиана
// kMeasuredRuns независимых замеров на каждую сторону.
// 5, не 11 (как в разовых диагностических бенчмарках этой сессии, см.
// anf/README.md) — здесь это часть штатного ctest, гоняемого на каждый
// PR/push (core/CONVENTIONS.md п.7а), и некоторые из бенчмаркуемых функций
// (aig_to_thr/anf_to_thr — ILP-solve, tt_to_anf — Мёбиус-трансформ) на
// kMaxReferenceAigVars=14 уже стоят ~1-2с ЗА ОДИН вызов — 11 повторов
// умножили бы время всего ctest-прогона на порядок. 5 — компромисс:
// заметно устойчивее одиночного замера (сама причина этого исправления),
// не взрывает время CI на дорогих функциях.
inline constexpr int kMeasuredRuns = 5;

// Прогревает `work` один раз (первый вызов часто содержит одноразовые
// издержки — аллокация TBB-арены, ленивая инициализация статиков, холодные
// статические кэши вроде get_ring() в aig_to_anf.cpp), затем замеряет
// МЕДИАНУ kMeasuredRuns повторов под 1-поточным global_control и столько
// же под дефолтным (все ядра), возвращает обе медианы и их отношение.
inline ScalingPoint measure_scaling(const std::string& size_label,
                                     const std::function<void()>& work) {
    work();  // прогрев — не входит в измерение

    ScalingPoint point;
    point.size_label = size_label;

    {
        oneapi::tbb::global_control limit(oneapi::tbb::global_control::max_allowed_parallelism, 1);
        std::vector<double> samples;
        samples.reserve(kMeasuredRuns);
        for (int i = 0; i < kMeasuredRuns; ++i) {
            const auto start = std::chrono::steady_clock::now();
            work();
            const auto end = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        }
        point.single_threaded_ms = detail::median_of(std::move(samples));
    }
    {
        // Без ограничения — oneTBB сам выбирает арену по числу ядер контейнера.
        std::vector<double> samples;
        samples.reserve(kMeasuredRuns);
        for (int i = 0; i < kMeasuredRuns; ++i) {
            const auto start = std::chrono::steady_clock::now();
            work();
            const auto end = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        }
        point.parallel_ms = detail::median_of(std::move(samples));
    }

    point.speedup = point.parallel_ms > 0.0 ? point.single_threaded_ms / point.parallel_ms : 0.0;
    return point;
}

}  // namespace bmm::benchmarks
