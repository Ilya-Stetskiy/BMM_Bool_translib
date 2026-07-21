// verify/full_matrix_tests.cpp — ПОЛНАЯ матрица корректности: для каждой
// упорядоченной пары представлений (X, Y) и для КАЖДОГО возможного
// промежуточного представления Z проверяется, совпадает ли результат
// прямого перевода X->Y и обходного X->Z->Y с независимо построенным
// эталоном того же логического представления (verify::reference_*, тот же
// принцип "не проверять код сам на себе", что и во всём verify/ —
// core/CONVENTIONS.md п.7).
//
// Отличие от verify/chain_tests.cpp ("Часть 3", прямой путь vs обходной):
// та часть — курируемый список ЗАВЕДОМО рабочих пар, и сравнивает только
// ВРЕМЯ (direct.ok/indirect.ok, но не сами значения). Здесь — ВСЕ пары без
// исключения, и главная проверка — СОВПАДЕНИЕ РЕЗУЛЬТАТА, а не факт, что
// перевод вообще выполнился.
//
// !!! ЭТОТ ТЕСТ МОЖЕТ ПОКАЗЫВАТЬ FAIL, И ЭТО СЕЙЧАС ОЖИДАЕМО !!!
// Ревью репозитория нашло: bdd_to_thr (bdd/bdd_to_thr.cpp) читает
// bdd.raw().TopVar() как индекс переменной напрямую, не вызывая
// bdd.var_at_level() — для Bdd с нестандартным порядком уровней
// (anf_to_bdd производит именно такой по умолчанию, FORCE-эвристика) это
// даёт молча неверные веса. Это отдельная, уже зафиксированная как задача
// находка (ветка исправления bdd_to_thr — item 2 в истории ревью), НЕ
// чинится этим файлом. Этот тест сделан специально, чтобы проблема была
// видна как воспроизводимый FAIL в `ctest`, а не только как абзац в
// отчёте ревью, который никто не перечитывает при каждом PR.
//
// ВРЕМЕННЫЙ таргет? Нет — как и chain_tests.cpp, этот файл ПОСТОЯННЫЙ (не
// удаляется после того как числа записаны): матрица актуальна всегда, пока
// в проекте есть 5 представлений и N функций трансляции между ними.

#include "verify/chain_utils.hpp"
#include "verify/ground_truth/ground_truth.hpp"
#include "verify/reference_builders.hpp"
#include "verify/test_runner.hpp"  // ground_truth_from_thr

#include <sylvan_obj.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace bmm;
using namespace bmm::verify;
using namespace bmm::chains;

namespace {

constexpr std::array<Repr, 5> kAllReprs = {Repr::Aig, Repr::Bdd, Repr::Anf, Repr::Thr, Repr::Tt};

// Независимый эталон логического представления r для функции, заданной
// одновременно как GroundTruthFunction (для Aig/Bdd/Anf/Tt через
// verify::reference_*) и как исходный Thr (для Repr::Thr — GroundTruthFunction
// сама была получена ИЗ этого Thr через ground_truth_from_thr, см. ниже, так
// что использовать его напрямую как эталон корректно, не тавтология: это тот
// же принцип, что thr_functions в chain_tests.cpp "1b").
std::optional<AnyRepr> reference_of(Repr r, const GroundTruthFunction& gt, const Thr& thr) {
    switch (r) {
        case Repr::Aig: { auto x = reference_aig(gt); if (!is_ok(x)) return std::nullopt; return AnyRepr(value(x)); }
        case Repr::Bdd: { auto x = reference_bdd(gt); if (!is_ok(x)) return std::nullopt; return AnyRepr(value(x)); }
        case Repr::Anf: { auto x = reference_anf(gt); if (!is_ok(x)) return std::nullopt; return AnyRepr(value(x)); }
        case Repr::Tt:  { auto x = reference_truth_table(gt); if (!is_ok(x)) return std::nullopt; return AnyRepr(value(x)); }
        case Repr::Thr: return AnyRepr(thr);
    }
    return std::nullopt;
}

// Для одной тестовой функции (заданной Thr — единственное представление,
// валидное для ВСЕХ пяти без исключения, остальные функции growing_test_
// functions() не обязаны быть пороговыми, а Thr не умеет представлять
// произвольную булеву функцию) — обходит ВСЕ (X, Y, Z), X!=Y, Z не in {X,Y}.
void run_full_matrix(Report& rep, const Thr& thr, const std::string& name) {
    GroundTruthFunction gt = ground_truth_from_thr(thr, name);

    for (Repr x : kAllReprs) {
        auto start = reference_of(x, gt, thr);
        if (!start) continue;

        for (Repr y : kAllReprs) {
            if (y == x) continue;
            auto reference_y = reference_of(y, gt, thr);
            if (!reference_y) continue;

            std::string prefix = name + " (n=" + std::to_string(gt.n_vars) + "): ";

            auto direct = run_chain(*start, {x, y});
            if (direct.ok) {
                bool mismatch = !reprs_equivalent(*direct.final_value, *reference_y);
                rep.bullet(!mismatch, prefix + repr_name(x) + "->" + repr_name(y) + " (прямой)" +
                                           (mismatch ? " — РАСХОДИТСЯ с независимым эталоном" : ""));
            }
            // direct.ok == false — пропуск (NotImplemented/Unsupported на этой паре для
            // этой конкретной функции), не ошибка: не любая функция обязана поддерживать
            // любой прямой перевод (см. bdd_to_thr K<=6, aig_to_thr только для пороговых
            // и т.п.) — рассчитываем total_checks только на реально выполнившихся путях,
            // как и во всём остальном verify/.

            for (Repr z : kAllReprs) {
                if (z == x || z == y) continue;
                auto indirect = run_chain(*start, {x, z, y});
                if (!indirect.ok) continue;  // тот же принцип: пропуск, не FAIL
                bool mismatch = !reprs_equivalent(*indirect.final_value, *reference_y);
                rep.bullet(!mismatch, prefix + repr_name(x) + "->" + repr_name(z) + "->" +
                                           repr_name(y) + " (обход)" +
                                           (mismatch ? " — РАСХОДИТСЯ с независимым эталоном" : ""));
            }
        }
    }
}

int g_result = 0;

VOID_TASK_0(full_matrix_main) {
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 22, 1LL << 26);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    Report rep;
    rep.md << "# Отчёт: полная матрица корректности всех пар/обходов (verify/full_matrix_tests.cpp)\n\n"
              "Автоматически перезаписывается при каждом запуске `test_full_matrix` — не "
              "редактировать руками.\n\n"
              "**Этот тест может показывать FAIL, и это сейчас ОЖИДАЕМО** — в отличие от "
              "`verify/chain_tests.cpp` (курируемый набор заведомо рабочих пар, только время), "
              "здесь проверяются ВСЕ упорядоченные пары представлений и ВСЕ промежуточные "
              "шаги без исключений — включая композиции, которые раньше нигде не "
              "проверялись. Известная причина текущих FAIL — `bdd_to_thr` игнорирует "
              "`Bdd::var_at_level()` для входов с нестандартным порядком уровней "
              "(таких, как выход `anf_to_bdd` по умолчанию) — отдельная, ещё не "
              "исправленная находка ревью. FAIL здесь реален и воспроизводим — не "
              "подавляйте его удалением проверки, чините исходную функцию.\n";

    auto thresholds = growing_threshold_test_functions(8);
    for (size_t i = 0; i < thresholds.size(); ++i) {
        std::string name = "thr#" + std::to_string(i);
        rep.section(name + " (n=" + std::to_string(thresholds[i].n_vars()) + ")");
        run_full_matrix(rep, thresholds[i], name);
    }

    rep.md << "\n## Итог\n\n" << rep.total_checks << " проверок, " << rep.failed_checks << " провалов.\n";
    std::printf("\n=== ИТОГ: %d проверок, %d провалов ===\n", rep.total_checks, rep.failed_checks);

    // Путь относительно рабочей директории процесса — тот же паттерн, что и
    // verify/chain_tests.cpp (WORKING_DIRECTORY=CMAKE_CURRENT_SOURCE_DIR у
    // этого таргета в CMakeLists.txt).
    std::ofstream out("verify/FULL_MATRIX_REPORT.md");
    if (out) {
        out << rep.md.str();
        std::printf("Отчёт записан в verify/FULL_MATRIX_REPORT.md\n");
    } else {
        std::printf("WARN: не удалось записать отчёт в файл (текущая директория не похожа на корень репозитория)\n");
    }

    g_result = rep.failed_checks == 0 ? 0 : 1;
    sylvan::sylvan_quit();
}

}  // namespace

int main() {
    const int n_workers = 0;
    // deque_size=1<<21 — см. подробное обоснование в verify/test_main.cpp
    // (эмпирически найденный и исправленный крах "Lace fatal error: Task
    // stack overflow" на реальном плотном ANF n=100/M=10000).
    const size_t deque_size = 1ULL << 21;
    lace_start(n_workers, deque_size);
    RUN(full_matrix_main);
    lace_stop();
    return g_result;
}
