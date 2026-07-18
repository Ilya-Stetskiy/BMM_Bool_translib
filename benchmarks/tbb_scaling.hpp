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
#include <ostream>
#include <string>
#include <vector>

#include <oneapi/tbb/global_control.h>

namespace bmm::benchmarks {

struct ScalingPoint {
    std::string size_label;
    double single_threaded_ms = 0.0;
    double parallel_ms = 0.0;
    double speedup = 0.0;  // single_threaded_ms / parallel_ms; >1 — параллельная версия быстрее
};

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

// Машиночитаемая строка для generate_status.sh (см. STATUS.md, раздел
// "Параллельность aig/anf") — тот же принцип, что и BMM_STATUS в
// verify/test_runner.hpp, но для результатов замера, не PASS/FAIL/SKIP.
inline void print_bench_line(const std::string& function_name, const ScalingPoint& point,
                              std::ostream& os) {
    os << "BMM_BENCH " << function_name << " size=" << point.size_label
       << " single_ms=" << point.single_threaded_ms << " parallel_ms=" << point.parallel_ms
       << " speedup=" << point.speedup << "\n";
}

// Итоговая строка по функции (после всех размеров) — вердикт, который
// реально попадёт в STATUS.md как одна строка на функцию (детальные
// per-size BMM_BENCH строки — для тех, кто хочет посмотреть глубже, в
// сыром выводе теста, не в STATUS.md).
inline void print_bench_summary(const std::string& function_name,
                                 const std::vector<ScalingPoint>& points, std::ostream& os) {
    if (points.empty()) {
        os << "BMM_BENCH_SUMMARY " << function_name << " нет данных\n";
        return;
    }
    double best_speedup = 0.0;
    for (const auto& p : points) best_speedup = std::max(best_speedup, p.speedup);

    os << "BMM_BENCH_SUMMARY " << function_name << " ";
    if (best_speedup > 1.05) {
        os << "параллельная версия дала ускорение до x" << best_speedup;
    } else if (best_speedup < 0.95 && best_speedup > 0.0) {
        os << "параллельная версия оказалась МЕДЛЕННЕЕ (x" << best_speedup
           << ") — см. известные ограничения, вероятно накладные расходы TBB "
              "перевешивают выигрыш на этих размерах входа";
    } else {
        os << "заметного эффекта от параллелизма не обнаружено (x" << best_speedup << ")";
    }
    os << "\n";
}

}  // namespace bmm::benchmarks
