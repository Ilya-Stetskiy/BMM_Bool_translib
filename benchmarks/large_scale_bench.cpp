// benchmarks/large_scale_bench.cpp — сравнительная таблица "однопоточно vs
// наш параллельный формат" для всех 12 функций трансляции, использующих
// TBB/OpenMP (core/CONVENTIONS.md п.6), БЕЗ ограничения в
// kMaxGroundTruthVars=12 — сколько реально позволяет каждая функция, не
// сколько позволяет перебор 2^n.
//
// Почему не growing_test_functions(): та строит std::vector<bool> размера
// 2^n на каждую функцию (verify/ground_truth/ground_truth.cpp) — при n в
// сотнях-тысячах, которые интересны здесь, это не то что медленно, а
// физически невозможно. Вместо "перебрать все возможные функции" (что сам
// пользователь справедливо назвал бессмысленным на таком масштабе) —
// другой метод:
//   1) генерация входа НАПРЯМУЮ в исходном представлении, O(размер), без
//      единого обращения к 2^n (см. benchmarks/large_scale_generators.hpp);
//   2) корректность проверяется не полным перебором и не сравнением с
//      internal ground truth, а НЕЗАВИСИМОЙ сверкой: результат под
//      однопоточной конфигурацией и результат под параллельной
//      конфигурацией ОБЯЗАНЫ совпасть на случайной выборке точек
//      (Aig::evaluate/Anf::evaluate/Thr::evaluate — O(размер) за вызов, не
//      O(2^n), поэтому сэмплирование дёшево даже при n=2000). Раньше
//      (benchmarks/tbb_scaling.hpp/openmp_scaling.hpp) single vs parallel
//      результаты друг с другом вообще не сравнивались — эти бенчмарки
//      мерили только время, полагаясь целиком на корректность, проверенную
//      отдельно при n<=12 (см. verify/test_runner.hpp). Здесь это
//      расхождение закрыто дополнительно, а не взамен.
//
// Не входит в ctest (см. обсуждение с пользователем) — отдельный
// опциональный бинарь, как benchmarks/bench_real_corpus.cpp. Пишет
// benchmarks/LARGE_SCALE_REPORT.md.

#include "aig/aig_to_anf.hpp"
#include "aig/aig_to_thr.hpp"
#include "aig/aig_to_tt.hpp"
#include "aig/tt_to_aig.hpp"
#include "anf/anf_to_aig.hpp"
#include "anf/anf_to_thr.hpp"
#include "anf/anf_to_tt.hpp"
#include "anf/tt_to_anf.hpp"
#include "thr/thr_to_aig.hpp"
#include "thr/thr_to_anf.hpp"
#include "thr/thr_to_tt.hpp"
#include "thr/tt_to_thr.hpp"

#include "benchmarks/large_scale_generators.hpp"
#include "benchmarks/scaling.hpp"

#include <kitty/constructors.hpp>
#include <kitty/dynamic_truth_table.hpp>

#include <oneapi/tbb/global_control.h>
#include <omp.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace bmm;
using namespace bmm::benchmarks;

namespace {

constexpr uint64_t kSeed = 20260719;
constexpr int kCorrectnessSamples = 30;
// Не растим n дальше, если один-единственный прогон уже дороже этого —
// иначе полный прогон всех 12 функций рискует занять часы вместо минут.
constexpr double kTimeBudgetMs = 15000.0;

double now_ms_diff(std::chrono::steady_clock::time_point t0, std::chrono::steady_clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// Сэмплированное сравнение: тот же принцип, что verify/chain_utils.hpp::
// reprs_equivalent_sampled, но здесь сравниваются ДВА результата ОДНОЙ и
// той же функции (однопоточный прогон против параллельного), а не два
// разных представления — поэтому просто T::evaluate() у обоих без
// какой-либо AnyRepr-машинерии.
template <class T>
bool sampled_equal(const T& a, const T& b, uint32_t n_vars, uint64_t seed) {
    std::mt19937_64 rng(seed);
    Assignment assignment(n_vars, false);
    for (int s = 0; s < kCorrectnessSamples; ++s) {
        for (uint32_t i = 0; i < n_vars; ++i) assignment[i] = (rng() & 1u) != 0;
        if (a.evaluate(assignment) != b.evaluate(assignment)) return false;
    }
    return true;
}

struct SweepRow {
    std::string function;
    std::string mechanism;
    uint32_t n_vars = 0;
    double single_ms = 0.0;
    double parallel_ms = 0.0;
    double speedup = 0.0;
    bool correctness_pass = false;
    std::string note;  // непусто <=> строка не PASS (SKIP/остановка roста)
};

std::vector<SweepRow> g_rows;

void print_row(const SweepRow& row) {
    if (!row.note.empty()) {
        std::printf("  %-12s %-8s n=%-6u %s\n", row.function.c_str(), row.mechanism.c_str(), row.n_vars,
                    row.note.c_str());
    } else {
        std::printf("  %-12s %-8s n=%-6u single=%9.2fмс parallel=%9.2fмс speedup=%6.2fx corr=%s\n",
                    row.function.c_str(), row.mechanism.c_str(), row.n_vars, row.single_ms, row.parallel_ms,
                    row.speedup, row.correctness_pass ? "PASS" : "FAIL");
    }
    std::fflush(stdout);
}

enum class Mechanism { Tbb, OpenMp, Sequential };

// Общий цикл прогона: строит вход через gen(n), проверяет один пробный
// вызов translate(input) (успех/явный отказ функции по её собственному
// статическому лимиту — останавливаем sweep для этой функции, дальше по n
// станет только хуже), затем меряет медиану kMeasuredRuns под 1 потоком и
// под дефолтной параллельной конфигурацией (TBB global_control/
// omp_set_num_threads в зависимости от mechanism; Sequential — тот же вызов
// дважды, честно покажет speedup~1x там, где в коде функции на деле нет
// ни TBB, ни OpenMP несмотря на конвенцию — см. шапку файла), и сверяет
// последний однопоточный результат с последним параллельным через
// sampled_equal.
template <class X, class Y, class GenFn, class TranslateFn>
void sweep(const char* func_name, Mechanism mech, const std::vector<uint32_t>& sizes, GenFn gen,
           TranslateFn translate) {
    const char* mech_name = mech == Mechanism::Tbb ? "TBB" : mech == Mechanism::OpenMp ? "OpenMP" : "seq";

    for (uint32_t n : sizes) {
        X input = gen(n);

        // ИСПРАВЛЕНО: раньше бюджет времени (kTimeBudgetMs) проверялся
        // ТОЛЬКО после полного 11-кратного замера (1 прогрев + 5+5
        // измеренных вызовов) — если СЛЕДУЮЩИЙ размер уже сам по себе
        // намного дороже предыдущего (эмпирически найдено на tt_to_anf:
        // n=20 -> 1.3с, n=22 -> 11с, рост ~x8 за шаг +2 — типично для
        // O(2^n)), 11 таких вызовов подряд могли занять на порядок больше
        // времени, чем сам бюджет, прежде чем проверка вообще срабатывала.
        // Пробный вызов теперь ТОЖЕ засекается — если он один уже дороже
        // бюджета, дальше не растим, не дожидаясь полного замера.
        auto probe_t0 = std::chrono::steady_clock::now();
        auto probe = translate(input);
        auto probe_t1 = std::chrono::steady_clock::now();
        double probe_ms = now_ms_diff(probe_t0, probe_t1);

        if (!is_ok(probe)) {
            SweepRow row{func_name, mech_name, n, 0, 0, 0, false,
                         std::string("SKIP (") + error(probe).message + ") -- дальше не растим"};
            print_row(row);
            g_rows.push_back(std::move(row));
            break;
        }
        if (probe_ms > kTimeBudgetMs) {
            SweepRow row{func_name, mech_name, n, 0, 0, 0, false,
                         "SKIP (пробный вызов уже " + std::to_string(probe_ms) +
                             "мс > бюджета) -- дальше не растим"};
            print_row(row);
            g_rows.push_back(std::move(row));
            break;
        }

        std::vector<double> single_samples, parallel_samples;
        single_samples.reserve(kMeasuredRuns);
        parallel_samples.reserve(kMeasuredRuns);
        Y single_value = value(probe);
        Y parallel_value = value(probe);

        auto run_measured = [&](std::vector<double>& samples, Y& last_value) {
            for (int i = 0; i < kMeasuredRuns; ++i) {
                auto t0 = std::chrono::steady_clock::now();
                auto r = translate(input);
                auto t1 = std::chrono::steady_clock::now();
                samples.push_back(now_ms_diff(t0, t1));
                last_value = value(r);
            }
        };

        translate(input);  // прогрев — не входит в измерение (см. tbb_scaling.hpp/openmp_scaling.hpp)

        if (mech == Mechanism::Tbb) {
            {
                oneapi::tbb::global_control limit(oneapi::tbb::global_control::max_allowed_parallelism, 1);
                run_measured(single_samples, single_value);
            }
            run_measured(parallel_samples, parallel_value);
        } else if (mech == Mechanism::OpenMp) {
            const int default_threads = omp_get_max_threads();
            omp_set_num_threads(1);
            run_measured(single_samples, single_value);
            omp_set_num_threads(default_threads);
            run_measured(parallel_samples, parallel_value);
        } else {
            run_measured(single_samples, single_value);
            run_measured(parallel_samples, parallel_value);
        }

        double single_ms = bmm::benchmarks::detail::median_of(std::move(single_samples));
        double parallel_ms = bmm::benchmarks::detail::median_of(std::move(parallel_samples));
        bool eq = sampled_equal(single_value, parallel_value, input.n_vars(), kSeed);

        SweepRow row{func_name, mech_name, n, single_ms, parallel_ms,
                     parallel_ms > 0.0 ? single_ms / parallel_ms : 0.0, eq, ""};
        print_row(row);
        g_rows.push_back(row);

        if (single_ms > kTimeBudgetMs) {
            std::printf("  %-12s -- бюджет времени (%.0fмс) исчерпан на n=%u, дальше не растим\n",
                        func_name, kTimeBudgetMs, n);
            break;
        }
    }
}

// TruthTable-производные генераторы (Tt на входе или выходе всегда
// ограничены kMaxTruthTableVars=24 самой структурой TruthTable — здесь без
// смысла расти дальше, но и лимит в 12 из growing_test_functions снят: идём
// до настоящего потолка). kitty::create_random заполняет таблицу случайными
// битами напрямую, без обхода точка-за-точкой через evaluate().
TruthTable random_tt(uint32_t n_vars, uint64_t seed) {
    kitty::dynamic_truth_table tt(n_vars);
    kitty::create_random(tt, seed);
    return TruthTable(tt);
}

void write_report(const std::string& path) {
    std::ofstream out(path);
    out << "# Отчёт: масштабирование однопоточно vs наш параллельный формат "
           "(benchmarks/large_scale_bench.cpp)\n\n"
           "Автоматически перезаписывается при каждом запуске "
           "`large_scale_bench` — не редактировать руками.\n\n"
           "Без ограничения в `kMaxGroundTruthVars=12` — размер входа растёт "
           "до собственного статического лимита функции (`TooManyVariables`) "
           "или до бюджета времени в 15с на один прогон, смотря что раньше. "
           "Корректность — не полный перебор (бессмысленен на таком n), а "
           "сверка однопоточного результата с параллельным на 30 случайных "
           "точках (`Assignment`), метод независим от полного перебора.\n\n"
           "| Функция | Механизм | n | Однопоточно, мс | Параллельно, мс | "
           "Ускорение | Корректность |\n"
           "|---|---|---|---|---|---|---|\n";
    for (const auto& row : g_rows) {
        if (!row.note.empty()) {
            out << "| " << row.function << " | " << row.mechanism << " | " << row.n_vars << " | "
                << row.note << " | | | |\n";
        } else {
            out << "| " << row.function << " | " << row.mechanism << " | " << row.n_vars << " | "
                << row.single_ms << " | " << row.parallel_ms << " | " << row.speedup << "x | "
                << (row.correctness_pass ? "PASS" : "FAIL") << " |\n";
        }
    }
    std::printf("Отчёт записан в %s\n", path.c_str());
}

}  // namespace

int main() {
    // --- TBB-функции (реально используют TBB, см. шапку файла) ---
    sweep<Aig, Anf>("aig_to_anf", Mechanism::Tbb, {50, 100, 200, 500, 1000, 2000},
                     [](uint32_t n) { return random_aig(n, n * 2, kSeed); },
                     [](const Aig& a) { return aig_to_anf(a); });

    sweep<Aig, Thr>("aig_to_thr", Mechanism::Tbb, {12, 16, 20, 24},
                     [](uint32_t n) { return random_aig(n, n * 2, kSeed); },
                     [](const Aig& a) { return aig_to_thr(a); });

    sweep<TruthTable, Aig>("tt_to_aig", Mechanism::Tbb, {12, 16, 20, 24},
                            [](uint32_t n) { return random_tt(n, kSeed); },
                            [](const TruthTable& t) { return tt_to_aig(t); });

    // --- Номинально TBB (core/CONVENTIONS.md), но фактически без единого
    // вызова TBB в теле функции (см. шапку файла и SESSION_REPORT.md) ---
    sweep<Anf, Aig>("anf_to_aig", Mechanism::Sequential, {50, 100, 200, 500, 1000, 2000},
                     [](uint32_t n) { return random_anf(n, n * 3, 3, kSeed); },
                     [](const Anf& a) { return anf_to_aig(a); });

    sweep<Anf, Thr>("anf_to_thr", Mechanism::Sequential, {12, 16, 18, 20},
                     [](uint32_t n) { return random_anf(n, n * 3, 3, kSeed); },
                     [](const Anf& a) { return anf_to_thr(a); });

    // --- OpenMP-функции ---
    // n=24 намеренно исключён для tt_to_anf: дважды эмпирически подтверждено
    // в этой сессии (и согласуется со старым TEST_TIMING_REPORT.md —
    // tt_to_anf_openmp_scaling/anf_to_tt_openmp_scaling там же занимали
    // 189-251с), что один вызов Anf::to_tt() (Мёбиус-преобразование внутри
    // tt_to_anf) при n=24 занимает минуты — probe-проверка бюджета времени
    // логически ловит это (см. sweep()), но САМ пробный вызов уже настолько
    // долгий, что успевает упереться во внешний timeout процесса раньше,
    // чем внутренняя проверка вообще получает шанс сработать. n=22 (11с) —
    // уже достаточный "практический потолок" для отчёта.
    sweep<TruthTable, Anf>("tt_to_anf", Mechanism::OpenMp, {16, 20, 22},
                            [](uint32_t n) { return random_tt(n, kSeed); },
                            [](const TruthTable& t) { return tt_to_anf(t); });

    sweep<Aig, TruthTable>("aig_to_tt", Mechanism::OpenMp, {12, 16, 20, 24},
                            [](uint32_t n) { return random_aig(n, n * 2, kSeed); },
                            [](const Aig& a) { return aig_to_tt(a); });

    sweep<Anf, TruthTable>("anf_to_tt", Mechanism::OpenMp, {12, 16, 20, 24},
                            [](uint32_t n) { return random_anf(n, n * 3, 3, kSeed); },
                            [](const Anf& a) { return anf_to_tt(a); });

    sweep<TruthTable, Thr>("tt_to_thr", Mechanism::OpenMp, {12, 16, 20},
                            [](uint32_t n) { return random_tt(n, kSeed); },
                            [](const TruthTable& t) { return tt_to_thr(t); });

    sweep<Thr, Anf>("thr_to_anf", Mechanism::OpenMp, {50, 100, 200, 500, 1000, 2000},
                     [](uint32_t n) { return random_thr(n, 50, kSeed); },
                     [](const Thr& t) { return thr_to_anf(t); });

    sweep<Thr, TruthTable>("thr_to_tt", Mechanism::OpenMp, {12, 16, 20, 24},
                            [](uint32_t n) { return random_thr(n, 50, kSeed); },
                            [](const Thr& t) { return thr_to_tt(t); });

    // --- Номинально OpenMP (thr/*), но фактически использует TBB (см.
    // шапку файла и находки этой сессии) ---
    sweep<Thr, Aig>("thr_to_aig", Mechanism::Tbb, {50, 100, 200, 500, 1000, 2000},
                     [](uint32_t n) { return random_thr(n, 50, kSeed); },
                     [](const Thr& t) { return thr_to_aig(t); });

    write_report("benchmarks/LARGE_SCALE_REPORT.md");
    return 0;
}
