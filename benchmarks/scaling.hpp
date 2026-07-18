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

// single_threaded_ms/parallel_ms — МЕДИАНА нескольких повторов (см.
// measure_scaling/measure_scaling_omp в tbb_scaling.hpp/openmp_scaling.hpp),
// не единичный замер. На суб-миллисекундных размерах (типичные тестовые
// функции n=8..16 из growing_test_functions) одиночный замер — это в
// основном шум OS-планировщика/холодного TBB-пула, а не сигнал о реальном
// параллелизме; единичный замер здесь систематически ненадёжен (проверено
// эмпирически — см. anf/README.md/aig/README.md о ложных 20x-90x
// "ускорениях", вызванных ровно этим).
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
//
// ИСПРАВЛЕНО (см. aig/README.md/anf/README.md): раньше здесь брался MAX
// speedup среди всех размеров — при шумных однократных замерах это
// систематически завышало впечатление о параллелизме, выбирая самый
// "везучий" размер, даже если остальные показывали ~1.0x или замедление
// (конкретный пример — aig_to_anf: {0.92, 1.27, 1.02} превращались в
// заголовок "ускорение до x1.27", хотя реального параллелизма в этой ветке
// нет вообще, см. aig/README.md §1.3). Теперь: МЕДИАНА speedup по всем
// размерам (устойчива к одному выбросу в любую сторону) + отдельная
// пометка, если знак эффекта (ускорение/замедление) не согласован между
// размерами — это само по себе сигнал "результат на грани шума", а не
// повод молча брать благоприятную интерпретацию.
inline void print_bench_summary(const std::string& function_name,
                                 const std::vector<ScalingPoint>& points, std::ostream& os) {
    if (points.empty()) {
        os << "BMM_BENCH_SUMMARY " << function_name << " нет данных\n";
        return;
    }

    std::vector<double> speedups;
    speedups.reserve(points.size());
    for (const auto& p : points) speedups.push_back(p.speedup);
    std::sort(speedups.begin(), speedups.end());
    const double median_speedup = speedups[speedups.size() / 2];

    const bool any_faster = std::any_of(points.begin(), points.end(),
                                         [](const ScalingPoint& p) { return p.speedup > 1.05; });
    const bool any_slower = std::any_of(points.begin(), points.end(),
                                         [](const ScalingPoint& p) { return p.speedup < 0.95 && p.speedup > 0.0; });
    const bool inconsistent = any_faster && any_slower;

    os << "BMM_BENCH_SUMMARY " << function_name << " ";
    if (inconsistent) {
        os << "результат НЕПОСЛЕДОВАТЕЛЕН между размерами (медиана x" << median_speedup
           << ") — вероятно эффект на грани шума измерения, не делайте выводов по одному размеру";
    } else if (median_speedup > 1.05) {
        os << "параллельная версия в среднем быстрее (медиана x" << median_speedup << ")";
    } else if (median_speedup < 0.95 && median_speedup > 0.0) {
        os << "параллельная версия в среднем МЕДЛЕННЕЕ (медиана x" << median_speedup
           << ") — см. известные ограничения, вероятно накладные расходы распараллеливания "
              "перевешивают выигрыш на этих размерах входа";
    } else {
        os << "заметного эффекта от параллелизма не обнаружено (медиана x" << median_speedup << ")";
    }
    os << "\n";
}

}  // namespace bmm::benchmarks
