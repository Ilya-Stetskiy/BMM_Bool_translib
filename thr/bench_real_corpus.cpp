// thr/bench_real_corpus.cpp — ПОСТОЯННЫЙ инструмент, тот же принцип, что
// anf/bench_real_corpus.cpp (см. комментарий там: не одноразовый bench_*.cpp,
// оставлен по прямому запросу пользователя — здесь тот же запрос, только
// про пороговые функции: "давай подцепим датасет реальных пороговых функций
// и будем с ними проводить тестирования").
//
// Зачем: verify::growing_threshold_test_functions() (см.
// verify/reference_builders.cpp) — это ЛИБО единичные веса (AND/OR/Majority),
// ЛИБО случайные веса из узкого диапазона [-5, 5]. Ни один из этих случаев
// не проверяет то, о чём прямо предупреждает thr_to_bdd.hpp — что размер BDD
// зависит не только от n, но и от того, НАСКОЛЬКО РАЗНЕСЕНЫ веса (унарные —
// полиномиально, экспоненциально разнесённые — потенциально нет). Нужен
// пример с реальными, содержательными, неравномерными весами.
//
// Источник — не выдуманные числа, а официально опубликованная взвешенная
// система голосования: Коллегия выборщиков США, 2024-2028 (веса — по
// переписи 2020 года). 50 штатов + округ Колумбия = 51 "переменная", вес
// каждой — число голосов выборщиков соответствующего штата, функция —
// sum(w_i * x_i) >= 270 из 538 (простое большинство). Официальный источник
// (сверено WebFetch при подготовке этого корпуса):
//   https://www.archives.gov/electoral-college/allocation
// Сумма всех 51 веса ниже равна 538 (проверено вручную, как и должно быть
// по самому определению системы — иначе таблица переписана неверно).
//
// n=51 > kMaxTruthTableVars (core/common.hpp, =24) — thr_to_anf/thr_to_tt/
// tt_to_thr контрактно не могут принять этот вход целиком (тот же лимит,
// что ограничивает их во всём остальном коде thr/), поэтому полная функция
// прогоняется только через thr_to_aig/thr_to_bdd (обе не завязаны на
// перечисление 2^n точек). Для оставшихся трёх функций используются
// РЕАЛЬНЫЕ ПРЕФИКСЫ того же списка (первые n штатов в порядке таблицы
// источника, те же веса) с порогом, пересчитанным как простое большинство
// именно этого префикса — веса по-прежнему настоящие, урезан только n.
//
// Сборка/запуск — тот же docker-образ, что и у anf/bench_real_corpus.cpp:
//   MSYS_NO_PATHCONV=1 docker run --rm -v "D:/Proga/web/bmm-translib:/workspace" \
//     -w /workspace genetica-boolean-lib:latest bash -c \
//     "cmake --build build --target bench_real_corpus_thr -j\$(nproc) && ./build/bench_real_corpus_thr"

#include "thr/thr_to_aig.hpp"
#include "thr/thr_to_anf.hpp"
#include "thr/thr_to_bdd.hpp"
#include "thr/thr_to_tt.hpp"
#include "thr/tt_to_thr.hpp"
#include "core/anf_repr.hpp"

#include <sylvan_obj.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace bmm;

namespace {

// Методология замера — тот же паттерн, что anf/bench_real_corpus.cpp и
// benchmarks/scaling.hpp: 1 прогрев + медиана kRuns повторов.
constexpr int kRuns = 11;

double median_ms(const std::function<void()>& work) {
    work();  // прогрев, не входит в измерение
    std::vector<double> samples;
    samples.reserve(kRuns);
    for (int i = 0; i < kRuns; ++i) {
        const auto start = std::chrono::steady_clock::now();
        work();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

// =============================================================================
// Коллегия выборщиков США, 2024-2028 (после переписи 2020 года). Порядок —
// как в таблице источника (алфавитный по названию штата), 50 штатов + округ
// Колумбия. Источник: National Archives,
// https://www.archives.gov/electoral-college/allocation (проверено при
// подготовке этого файла; официальный итог по определению — 538 голосов,
// большинство — 270; сумма весов ниже сверена вручную и равна 538).
// =============================================================================

struct ElectoralEntry {
    const char* name;
    int64_t electors;
};

constexpr ElectoralEntry kElectoralCollege2024[] = {
    {"Alabama", 9},          {"Alaska", 3},          {"Arizona", 11},        {"Arkansas", 6},
    {"California", 54},      {"Colorado", 10},       {"Connecticut", 7},     {"Delaware", 3},
    {"District of Columbia", 3}, {"Florida", 30},     {"Georgia", 16},       {"Hawaii", 4},
    {"Idaho", 4},             {"Illinois", 19},       {"Indiana", 11},       {"Iowa", 6},
    {"Kansas", 6},            {"Kentucky", 8},        {"Louisiana", 8},      {"Maine", 4},
    {"Maryland", 10},         {"Massachusetts", 11},  {"Michigan", 15},      {"Minnesota", 10},
    {"Mississippi", 6},       {"Missouri", 10},       {"Montana", 4},        {"Nebraska", 5},
    {"Nevada", 6},            {"New Hampshire", 4},   {"New Jersey", 14},    {"New Mexico", 5},
    {"New York", 28},         {"North Carolina", 16}, {"North Dakota", 3},   {"Ohio", 17},
    {"Oklahoma", 7},          {"Oregon", 8},          {"Pennsylvania", 19},  {"Rhode Island", 4},
    {"South Carolina", 9},    {"South Dakota", 3},    {"Tennessee", 11},     {"Texas", 40},
    {"Utah", 6},              {"Vermont", 3},         {"Virginia", 13},      {"Washington", 12},
    {"West Virginia", 4},     {"Wisconsin", 10},      {"Wyoming", 3},
};
constexpr size_t kNumStates = sizeof(kElectoralCollege2024) / sizeof(kElectoralCollege2024[0]);
static_assert(kNumStates == 51, "50 штатов + округ Колумбия");

Thr build_electoral_college_full() {
    std::vector<int64_t> weights;
    weights.reserve(kNumStates);
    int64_t total = 0;
    for (const auto& e : kElectoralCollege2024) {
        weights.push_back(e.electors);
        total += e.electors;
    }
    if (total != 538) {
        std::fprintf(stderr, "BMM_REAL_BENCH ОШИБКА: сумма голосов выборщиков = %lld, ожидалось 538\n",
                     static_cast<long long>(total));
        std::abort();
    }
    return Thr(std::move(weights), 270);
}

// Реальный ПРЕФИКС того же списка (первые n штатов в порядке таблицы
// источника, те же веса) — веса не выдуманы, урезан только n, чтобы
// уместиться под kMaxTruthTableVars для thr_to_anf/thr_to_tt/tt_to_thr.
// theta = простое большинство ИМЕННО этого префикса (270 из полного списка
// не достижимо на подмножестве весов).
Thr build_electoral_prefix(uint32_t n) {
    std::vector<int64_t> weights;
    weights.reserve(n);
    int64_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        weights.push_back(kElectoralCollege2024[i].electors);
        total += kElectoralCollege2024[i].electors;
    }
    return Thr(std::move(weights), total / 2 + 1);
}

// Число мономов ANF — та же логика, что anf/bench_real_corpus.cpp
// (count_monomials()): нужна, чтобы проверить гипотезу из anf/README.md §9.3
// ("poly += mono при сборке ANF из коэффициентов ТТ — потенциально
// квадратичный по числу мономов — не подтверждена и не опровергнута: во всём
// корпусе не было по-настоящему плотного входа при n=20-24"). Пороговые
// функции по определению плотные (thr_to_anf.hpp: "степень ANF растёт с n,
// известных компактных формул нет") — это именно тот недостающий случай.
size_t count_monomials(const Anf& anf) {
#if BMM_HAVE_BRIAL
    size_t c = 0;
    for (auto it = anf.raw().begin(); it != anf.raw().end(); ++it) ++c;
    return c;
#else
    return anf.raw().monomials().size();
#endif
}

template <class F, class In>
void bench_one(const std::string& label, uint32_t n, F&& fn, const In& input) {
    const double ms = median_ms([&] {
        auto r = fn(input);
        (void)r;
    });
    std::printf("BMM_REAL_BENCH %-10s n=%-3u median_ms=%.4f\n", label.c_str(), n, ms);
}

}  // namespace

void run_all() {
    std::printf("=== thr/ real-corpus benchmark: Коллегия выборщиков США (2024-2028) ===\n\n");

    // -------------------------------------------------------------------
    // Полная функция, n=51: только thr_to_aig/thr_to_bdd (n > kMaxTruthTableVars).
    // -------------------------------------------------------------------
    std::printf("--- полная коллегия выборщиков, n=51, theta=270/538 ---\n");
    {
        Thr full = build_electoral_college_full();

        const double aig_ms = median_ms([&] {
            auto r = thr_to_aig(full);
            (void)r;
        });
        auto aig_result = thr_to_aig(full);
        if (is_ok(aig_result)) {
            std::printf("BMM_REAL_BENCH thr_to_aig n=51 gates=%zu median_ms=%.4f\n",
                        static_cast<size_t>(value(aig_result).raw().num_gates()), aig_ms);
        } else {
            std::printf("BMM_REAL_BENCH thr_to_aig n=51 FAILED\n");
        }

        const double bdd_ms = median_ms([&] {
            auto r = thr_to_bdd(full);
            (void)r;
        });
        auto bdd_result = thr_to_bdd(full);
        if (is_ok(bdd_result)) {
            std::printf("BMM_REAL_BENCH thr_to_bdd n=51 nodes=%zu median_ms=%.4f\n",
                        value(bdd_result).raw().NodeCount(), bdd_ms);
        } else {
            std::printf("BMM_REAL_BENCH thr_to_bdd n=51 FAILED\n");
        }
    }

    // -------------------------------------------------------------------
    // Растущие РЕАЛЬНЫЕ префиксы (n=8,12,16,20,24) — все 5 функций,
    // включая round-trip tt_to_thr(thr_to_tt(...)).
    // -------------------------------------------------------------------
    std::printf("\n--- реальные префиксы коллегии выборщиков (растущий n) ---\n");
    for (uint32_t n : {8u, 12u, 16u, 20u, 24u}) {
        Thr prefix = build_electoral_prefix(n);
        std::printf("-- n=%u, theta=%lld --\n", n, static_cast<long long>(prefix.theta()));

        bench_one("thr_to_aig", n, [](const Thr& t) { return thr_to_aig(t); }, prefix);
        bench_one("thr_to_bdd", n, [](const Thr& t) { return thr_to_bdd(t); }, prefix);

        {
            const double anf_ms = median_ms([&] {
                auto r = thr_to_anf(prefix);
                (void)r;
            });
            auto anf_result = thr_to_anf(prefix);
            if (is_ok(anf_result)) {
                std::printf("BMM_REAL_BENCH thr_to_anf n=%-3u monomials=%-8zu median_ms=%.4f\n", n,
                            count_monomials(value(anf_result)), anf_ms);
            } else {
                std::printf("BMM_REAL_BENCH thr_to_anf n=%u FAILED\n", n);
            }
        }

        bench_one("thr_to_tt", n, [](const Thr& t) { return thr_to_tt(t); }, prefix);

        // tt_to_thr — ТОЛЬКО n<=16: на реальных (не единичных) весах ILP-решатель
        // взрывается гораздо быстрее, чем 2^n сам по себе — измерено в первом
        // прогоне этого корпуса: median_ms 19.9 (n=8) -> 280.3 (n=12) -> 11119.4
        // (n=16, т.е. >11 СЕКУНД на один вызов) -> на n=20 отдельный вызов не
        // уложился в разумное время прогона (процесс остановлен вручную после
        // нескольких минут на первом же из 12 повторов) — рост явно
        // сверхэкспоненциальный по сравнению с thr_to_tt на том же n (у той —
        // 2.1 -> 2.1 -> 2.1 -> 39.5 мс на n=8/12/16/20). Само по себе это —
        // отдельная находка про tt_to_thr (см. код-ревью): ILP/SCIP-подход
        // (thr/tt_to_thr.cpp) на реальных весах непрактичен уже при n~16-20,
        // задолго до формального лимита n>=32 в самом коде.
        if (n <= 16) {
            auto tt_result = thr_to_tt(prefix);
            if (is_ok(tt_result)) {
                const TruthTable& tt = value(tt_result);
                bench_one("tt_to_thr", n, [](const TruthTable& t) { return tt_to_thr(t); }, tt);
                auto back = tt_to_thr(tt);
                std::printf("BMM_REAL_CHECK tt_to_thr n=%u %s\n", n,
                            is_ok(back) ? "распознана как пороговая (ожидаемо)"
                                        : "НЕ распознана как пороговая (неожиданно — вход по построению пороговый)");
            } else {
                std::printf("BMM_REAL_BENCH tt_to_thr n=%u SKIP (thr_to_tt FAILED)\n", n);
            }
        } else {
            std::printf(
                "BMM_REAL_BENCH tt_to_thr  n=%-3u ПРОПУЩЕНО (известный сверхэкспоненциальный рост "
                "времени ILP/SCIP на реальных весах, см. комментарий выше)\n",
                n);
        }
    }

    std::printf("\n=== done ===\n");
}

// Sylvan/Lace требует, чтобы весь код, трогающий sylvan::Bdd (внутри
// thr_to_bdd), выполнялся внутри зарегистрированной Lace-задачи — тот же
// паттерн, что anf/bench_real_corpus.cpp (см. комментарий там).

namespace {
VOID_TASK_0(run_all_task) {
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 22, 1LL << 26);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    run_all();

    sylvan::sylvan_quit();
}
}  // namespace

int main() {
    const int n_workers = 0;
    const size_t deque_size = 0;
    lace_start(n_workers, deque_size);
    RUN(run_all_task);
    lace_stop();
    return 0;
}
