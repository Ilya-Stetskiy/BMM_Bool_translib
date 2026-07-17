// benchmarks/scaling.hpp — формат результата и печать, ОБЩИЕ для всех
// backend'ов параллелизма (TBB, OpenMP). Сам замер (как именно заставить
// код выполниться в 1 поток vs по умолчанию) специфичен для backend'а и
// живёт в benchmarks/tbb_scaling.hpp / benchmarks/openmp_scaling.hpp —
// каждый из них возвращает ScalingPoint и печатает его этими же функциями,
// поэтому строки BMM_BENCH/BMM_BENCH_SUMMARY в выводе теста выглядят
// одинаково независимо от того, какой backend использует конкретная
// функция трансляции (см. core/CONVENTIONS.md п.6 за тем, что кому
// положено).

#pragma once

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

namespace bmm::benchmarks {

struct ScalingPoint {
    std::string size_label;
    double single_threaded_ms = 0.0;
    double parallel_ms = 0.0;
    double speedup = 0.0;  // single_threaded_ms / parallel_ms; >1 — параллельная версия быстрее
};

// Машиночитаемая строка для generate_status.sh (см. STATUS.md, раздел
// "Параллельность") — тот же принцип, что и BMM_STATUS в
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
// сыром выводе теста, не в STATUS.md). Формат идентичен для TBB и OpenMP —
// студент/преподаватель видит одну и ту же по структуре строку
// "такая-то функция так-то ускорена" вне зависимости от backend'а.
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
           << ") — см. известные ограничения, вероятно накладные расходы распараллеливания "
              "перевешивают выигрыш на этих размерах входа";
    } else {
        os << "заметного эффекта от параллелизма не обнаружено (x" << best_speedup << ")";
    }
    os << "\n";
}

}  // namespace bmm::benchmarks
