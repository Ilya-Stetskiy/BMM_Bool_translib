// Временный диагностический бинарь — проверить парсер benchmarks/
// anf_dimacs_loader.hpp на реальном ANF-корпусе persons.iis.nsk.su
// (benchmarks/data/iis-nsk/100-10k-rnd/*.anf, n=100, 10000 мономов) и
// посмотреть, как ведут себя anf_to_aig/anf_to_tt/anf_to_thr на РЕАЛЬНОМ
// (не синтетическом chi/bent) плотном ANF при n=100 — существенно больше,
// чем что-либо, использованное в SESSION_REPORT.md ранее (макс. n=64 у chi).
//
// НЕ включает anf_to_bdd — по опыту этой же сессии (bent_mm_n40, ~48с даже
// с FORCE) и предыдущей (структурный взрыв anf_to_bdd/aig_to_bdd без
// защиты), запуск на n=100/M=10000 с ПОЧТИ ПОЛНЫМ графом взаимодействия
// переменных (см. диагностику при чтении файла — средняя длина монома ~65
// из 100) рискует не завершиться за разумное время или уйти в Lace abort()
// (не ловится изнутри процесса) — см. отдельный diag_anf_to_bdd.cpp,
// запускаемый ТОЛЬКО под внешним `timeout`.

#include "benchmarks/anf_dimacs_loader.hpp"
#include "verify/chain_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace bmm;
using namespace bmm::benchmarks;
using namespace bmm::chains;

namespace {

void check_one(const std::string& path) {
    std::printf("== %s ==\n", path.c_str());
    std::fflush(stdout);

    auto anf = load_anf_dimacs(path);
    if (!anf) {
        std::printf("FAIL: не удалось распарсить\n");
        std::fflush(stdout);
        return;
    }
    std::printf("PASS парсинг: n_vars=%u\n", anf->n_vars());
    std::fflush(stdout);

    AnyRepr anf_repr = AnyRepr(*anf);

    // anf_to_tt: ожидаемый отказ (n=100 >> kMaxTruthTableVars=24).
    {
        auto r = anf_to_tt(*anf);
        if (is_ok(r)) {
            std::printf("НЕОЖИДАННО: anf_to_tt() succeeded at n=%u\n", anf->n_vars());
        } else {
            std::printf("OK (ожидаемый отказ) anf_to_tt: %s\n", error(r).message.c_str());
        }
        std::fflush(stdout);
    }

    // anf_to_thr: ожидаемый отказ (либо "не пороговая функция", либо лимит
    // переменных для ILP/LP).
    {
        auto r = anf_to_thr(*anf);
        if (is_ok(r)) {
            std::printf("НЕОЖИДАННО: anf_to_thr() succeeded at n=%u\n", anf->n_vars());
        } else {
            std::printf("OK (ожидаемый отказ) anf_to_thr: %s\n", error(r).message.c_str());
        }
        std::fflush(stdout);
    }

    // anf_to_aig: единственная функция без статического n-лимита и без
    // известного риска структурного взрыва (в отличие от anf_to_bdd) — по
    // aig/README.md "не найдено ни одного отказа" (найдено ИСКЛЮЧЕНИЕ для
    // aig_to_anf на реальной схеме в этой же сессии, но НЕ для anf_to_aig).
    {
        auto t0 = std::chrono::steady_clock::now();
        auto r = anf_to_aig(*anf);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (!is_ok(r)) {
            std::printf("FAIL anf_to_aig: %s\n", error(r).message.c_str());
            std::fflush(stdout);
            return;
        }
        std::printf("PASS anf_to_aig: %.3f мс, n_gates=%zu\n", ms,
                    static_cast<size_t>(value(r).raw().num_gates()));
        std::fflush(stdout);

        AnyRepr aig_repr = AnyRepr(value(r));

        // Anf::evaluate() — O(размер полинома) НА КАЖДЫЙ вызов (прямой
        // проход по термам, см. core/anf_repr.hpp) — на M=10000 мономов
        // средней длины ~65 это уже не бесплатно, в отличие от chi/bent
        // (3-50 мономов), использованных в verify/real_datasets_tests.cpp
        // ранее в этой сессии. reprs_equivalent_sampled из verify/
        // chain_utils.hpp вызывает Anf::evaluate() напрямую (НЕ через
        // быстрый to_tt()-based evaluator — то невозможно при n=100), и на
        // фиксированных 2000 точках это заняло больше 120с (первая попытка
        // этого диагностика зависла именно здесь, а не на построении AIG).
        // Калибруем число точек под фактическую стоимость вместо
        // фиксированного количества — тот же принцип, что и предыдущая
        // находка сессии про Anf::to_tt() (systemic O(4^n)-подобный сбой в
        // test-инфраструктуре), только здесь дешевле адаптировать число
        // сэмплов, чем чинить сам Anf::evaluate() (уже минимальная
        // асимптотика для прямого вычисления без предпосчёта, которого при
        // n=100 всё равно нет — to_tt() недоступен).
        Assignment probe(anf->n_vars(), false);
        auto tc0 = std::chrono::steady_clock::now();
        constexpr int kCalibrationRuns = 5;
        for (int i = 0; i < kCalibrationRuns; ++i) {
            probe[static_cast<size_t>(i) % probe.size()] = true;
            volatile bool a = anf->evaluate(probe);
            volatile bool b = value(r).evaluate(probe);
            (void)a; (void)b;
        }
        auto tc1 = std::chrono::steady_clock::now();
        double per_sample_ms =
            std::chrono::duration<double, std::milli>(tc1 - tc0).count() / kCalibrationRuns;
        const double kBudgetMs = 5000.0;
        uint64_t num_samples =
            static_cast<uint64_t>(std::max(20.0, std::min(2000.0, kBudgetMs / std::max(per_sample_ms, 0.001))));
        std::printf("  (калибровка: ~%.3f мс/сэмпл, выбрано %llu сэмплов)\n", per_sample_ms,
                    static_cast<unsigned long long>(num_samples));
        std::fflush(stdout);

        bool eq = reprs_equivalent_sampled(anf_repr, aig_repr, num_samples, 20260719);
        std::printf("%s anf_to_aig round-trip-free эквивалентность (выборка %llu точек)\n",
                    eq ? "PASS" : "FAIL", static_cast<unsigned long long>(num_samples));
        std::fflush(stdout);

        // Round-trip обратно (aig_to_anf): ИСПРАВЛЕНО — раньше зависал
        // (>180с) на этом же 73646-гейтовом AIG (структурный взрыв самого
        // ANF-представления, не порядка обхода — см. комментарий у
        // kMaxAnfMonomialsDuringConstruction в aig/aig_to_anf.cpp). Теперь
        // должен вернуться быстро с честным отказом вместо зависания.
        auto t2 = std::chrono::steady_clock::now();
        auto back = aig_to_anf(value(r));
        auto t3 = std::chrono::steady_clock::now();
        double ms_back = std::chrono::duration<double, std::milli>(t3 - t2).count();
        if (!is_ok(back)) {
            std::printf("OK (ожидаемый отказ, было зависание) aig_to_anf: %s (%.3f мс)\n",
                        error(back).message.c_str(), ms_back);
        } else {
            std::printf("НЕОЖИДАННО: aig_to_anf succeeded (%.3f мс)\n", ms_back);
        }
        std::fflush(stdout);
    }

    std::printf("\n");
    std::fflush(stdout);
}

}  // namespace

int main() {
    std::vector<std::string> files = {
        "benchmarks/data/iis-nsk/100-10k-rnd/10-100x90-100.01.anf",
        "benchmarks/data/iis-nsk/100-10k-rnd/50-50x50-200.01.anf",
        "benchmarks/data/iis-nsk/100-10k-rnd/90-10x10-1000.01.anf",
    };
    for (const auto& f : files) check_one(f);
    return 0;
}
