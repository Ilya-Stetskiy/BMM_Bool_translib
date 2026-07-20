// verify/chain_tests.cpp — тесты КОМПОЗИЦИИ переводов между представлениями,
// в дополнение к тестам ОТДЕЛЬНЫХ функций в test_aig/test_bdd/test_anf/
// test_thr.cpp:
//
//  1. Round-trip (X -> Y -> X): для каждой осмысленной пары представлений —
//     сохраняется ли функция после перевода туда и обратно? Для точных
//     представлений (без потери информации) это должно быть тождеством;
//     расхождение — признак бага в одной из двух вовлечённых функций
//     трансляции (или в паре сразу, если ошибки взаимно компенсируются).
//  2. Более длинные циклы (3-5 шагов) — та же идея глубже: ошибка, которая
//     "случайно" компенсируется на перевода-и-обратно, с большей вероятностью
//     проявится на цикле через несколько представлений.
//  3. Прямой перевод (X->Y) против перевода через третье представление
//     (X->Z->Y) — где обходной путь быстрее прямого и почему.
//
// Постоянный файл (не удаляется после прогона, как bench_real_corpus.cpp) —
// результат (verify/CHAIN_TESTS_REPORT.md) перезаписывается при каждом
// запуске, как STATUS.md.

#include "verify/chain_utils.hpp"
#include "verify/ground_truth/ground_truth.hpp"
#include "verify/reference_builders.hpp"

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

std::optional<AnyRepr> build_repr(Repr r, const GroundTruthFunction& gt) {
    switch (r) {
        case Repr::Aig: {
            auto x = reference_aig(gt);
            if (!is_ok(x)) return std::nullopt;
            return AnyRepr(value(x));
        }
        case Repr::Bdd: {
            auto x = reference_bdd(gt);
            if (!is_ok(x)) return std::nullopt;
            return AnyRepr(value(x));
        }
        case Repr::Anf: {
            auto x = reference_anf(gt);
            if (!is_ok(x)) return std::nullopt;
            return AnyRepr(value(x));
        }
        case Repr::Tt: {
            auto x = reference_truth_table(gt);
            if (!is_ok(x)) return std::nullopt;
            return AnyRepr(value(x));
        }
        case Repr::Thr:
            return std::nullopt;  // не любая функция пороговая — см. is_known_threshold_by_name
    }
    return std::nullopt;
}

// Пороговые ПО ПОСТРОЕНИЮ имена из verify::growing_test_functions
// (ground_truth.cpp) — используется, чтобы включать Thr в цепочки только на
// заведомо совместимых функциях, не полагаясь на ILP-решатель "угадать" это.
bool is_known_threshold_by_name(const std::string& name) {
    auto starts_with = [&](const char* prefix) { return name.rfind(prefix, 0) == 0; };
    return starts_with("const0") || starts_with("const1") || starts_with("proj_") ||
           starts_with("and_all") || starts_with("or_all") || starts_with("maj_");
}

// ---------------------------------------------------------------------------
// Отчёт: печатает в stdout по ходу дела и копит markdown для файла.
// ---------------------------------------------------------------------------

struct Report {
    std::ostringstream md;
    int total_checks = 0;
    int failed_checks = 0;

    void section(const std::string& title) {
        md << "\n## " << title << "\n\n";
        std::printf("\n=== %s ===\n", title.c_str());
    }

    void bullet(bool ok, const std::string& text) {
        ++total_checks;
        if (!ok) ++failed_checks;
        md << "- **" << (ok ? "PASS" : "FAIL") << "** " << text << "\n";
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text.c_str());
    }

    void line(const std::string& text) {
        md << text << "\n";
        std::printf("%s\n", text.c_str());
    }

    void table_row(const std::vector<std::string>& cells) {
        md << "|";
        for (const auto& c : cells) md << " " << c << " |";
        md << "\n";
    }
};

// ---------------------------------------------------------------------------
// Часть 1: round-trip X -> Y -> X
// ---------------------------------------------------------------------------

void run_round_trips(Report& rep) {
    rep.section("1. Round-trip (X -> Y -> X): сохраняется ли функция?");

    const std::vector<Repr> kGeneral = {Repr::Aig, Repr::Bdd, Repr::Anf, Repr::Tt};
    auto functions = growing_test_functions(10);

    // 1a. Все 12 упорядоченных пар среди {Aig, Bdd, Anf, Tt} — на всём наборе
    // growing_test_functions(10) (константы, проекции, AND/OR/XOR-всех, MAJ,
    // случайные), без ограничения на пороговость.
    for (Repr x : kGeneral) {
        for (Repr y : kGeneral) {
            if (x == y) continue;
            int pass = 0, mismatch = 0, skip = 0;
            double total_ms = 0.0;
            std::string first_mismatch_name;
            for (const auto& gt : functions) {
                auto start = build_repr(x, gt);
                if (!start) { ++skip; continue; }
                auto chain = run_chain(*start, {x, y, x});
                if (!chain.ok) { ++skip; continue; }
                total_ms += chain.total_ms;
                if (reprs_equivalent(*start, *chain.final_value)) {
                    ++pass;
                } else {
                    ++mismatch;
                    if (first_mismatch_name.empty()) first_mismatch_name = gt.name;
                }
            }
            std::ostringstream text;
            text << repr_name(x) << " -> " << repr_name(y) << " -> " << repr_name(x) << ": "
                 << pass << " ок, " << mismatch << " разошлось, " << skip << " пропущено ("
                 << total_ms << " мс суммарно)";
            if (mismatch > 0) text << " — первое расхождение: " << first_mismatch_name;
            rep.bullet(mismatch == 0, text.str());
        }
    }

    // 1b. Thr -> X -> Thr, на реальных Thr-объектах (growing_threshold_test_functions).
    auto thr_functions = growing_threshold_test_functions(10);
    for (Repr x : kGeneral) {
        int pass = 0, mismatch = 0, skip = 0;
        double total_ms = 0.0;
        std::string first_mismatch_name;
        for (const auto& thr : thr_functions) {
            AnyRepr start = AnyRepr(thr);
            auto chain = run_chain(start, {Repr::Thr, x, Repr::Thr});
            if (!chain.ok) { ++skip; continue; }
            total_ms += chain.total_ms;
            if (reprs_equivalent(start, *chain.final_value)) {
                ++pass;
            } else {
                ++mismatch;
                if (first_mismatch_name.empty())
                    first_mismatch_name = "n_vars=" + std::to_string(thr.n_vars());
            }
        }
        std::ostringstream text;
        text << "Thr -> " << repr_name(x) << " -> Thr: " << pass << " ок, " << mismatch
             << " разошлось, " << skip << " пропущено (" << total_ms << " мс суммарно)";
        if (mismatch > 0) text << " — первое расхождение: " << first_mismatch_name;
        rep.bullet(mismatch == 0, text.str());
    }

    // 1c. X -> Thr -> X, на функциях, заведомо пороговых по построению
    // (const/proj/and_all/or_all/maj) — иначе X->Thr корректно вернёт
    // Unsupported, и это не баг, а пропуск случая.
    for (Repr x : kGeneral) {
        int pass = 0, mismatch = 0, skip = 0;
        double total_ms = 0.0;
        for (const auto& gt : functions) {
            if (!is_known_threshold_by_name(gt.name)) continue;
            auto start = build_repr(x, gt);
            if (!start) { ++skip; continue; }
            auto chain = run_chain(*start, {x, Repr::Thr, x});
            if (!chain.ok) { ++skip; continue; }
            total_ms += chain.total_ms;
            if (reprs_equivalent(*start, *chain.final_value)) {
                ++pass;
            } else {
                ++mismatch;
            }
        }
        std::ostringstream text;
        text << repr_name(x) << " -> Thr -> " << repr_name(x) << " (на заведомо пороговых функциях): "
             << pass << " ок, " << mismatch << " разошлось, " << skip << " пропущено (" << total_ms
             << " мс суммарно)";
        rep.bullet(mismatch == 0, text.str());
    }
}

// ---------------------------------------------------------------------------
// Часть 2: более длинные циклы (3-5 шагов)
// ---------------------------------------------------------------------------

void run_longer_cycles(Report& rep) {
    rep.section("2. Более длинные циклы (3-5 переводов подряд)");

    // 2a. Полный 5-шаговый цикл через ВСЕ пять представлений и обратно к
    // старту, на заведомо пороговых функциях (иначе шаг ->Thr не пройдёт).
    {
        auto functions = growing_test_functions(8);
        int pass = 0, mismatch = 0, skip = 0;
        for (const auto& gt : functions) {
            if (!is_known_threshold_by_name(gt.name)) continue;
            auto start = build_repr(Repr::Aig, gt);
            if (!start) { ++skip; continue; }
            auto chain =
                run_chain(*start, {Repr::Aig, Repr::Bdd, Repr::Anf, Repr::Thr, Repr::Tt, Repr::Aig});
            if (!chain.ok) { ++skip; continue; }
            if (reprs_equivalent(*start, *chain.final_value)) ++pass; else ++mismatch;
        }
        rep.bullet(mismatch == 0, "Aig->Bdd->Anf->Thr->Tt->Aig (5 шагов, пороговые функции n<=8): " +
                                       std::to_string(pass) + " ок, " + std::to_string(mismatch) +
                                       " разошлось, " + std::to_string(skip) + " пропущено");
    }

    // 2b. 4-шаговый цикл без Thr, на ОБЩИХ функциях (включая XOR/random —
    // не обязаны быть пороговыми).
    {
        auto functions = growing_test_functions(10);
        int pass = 0, mismatch = 0, skip = 0;
        for (const auto& gt : functions) {
            auto start = build_repr(Repr::Anf, gt);
            if (!start) { ++skip; continue; }
            auto chain = run_chain(*start, {Repr::Anf, Repr::Bdd, Repr::Aig, Repr::Tt, Repr::Anf});
            if (!chain.ok) { ++skip; continue; }
            if (reprs_equivalent(*start, *chain.final_value)) ++pass; else ++mismatch;
        }
        rep.bullet(mismatch == 0, "Anf->Bdd->Aig->Tt->Anf (4 шага, любые функции n<=10): " +
                                       std::to_string(pass) + " ок, " + std::to_string(mismatch) +
                                       " разошлось, " + std::to_string(skip) + " пропущено");
    }

    // 2c. 3-шаговый цикл, другой порядок обхода.
    {
        auto functions = growing_test_functions(10);
        int pass = 0, mismatch = 0, skip = 0;
        for (const auto& gt : functions) {
            auto start = build_repr(Repr::Bdd, gt);
            if (!start) { ++skip; continue; }
            auto chain = run_chain(*start, {Repr::Bdd, Repr::Tt, Repr::Anf, Repr::Bdd});
            if (!chain.ok) { ++skip; continue; }
            if (reprs_equivalent(*start, *chain.final_value)) ++pass; else ++mismatch;
        }
        rep.bullet(mismatch == 0, "Bdd->Tt->Anf->Bdd (3 шага, любые функции n<=10): " +
                                       std::to_string(pass) + " ок, " + std::to_string(mismatch) +
                                       " разошлось, " + std::to_string(skip) + " пропущено");
    }
}

// ---------------------------------------------------------------------------
// Часть 3: прямой перевод против перевода через третье представление
// ---------------------------------------------------------------------------

void run_indirect_vs_direct(Report& rep) {
    rep.section("3. Прямой перевод (X->Y) против обхода через третье представление (X->Z->Y)");

    struct Case {
        Repr from, to;
        std::vector<Repr> via;  // кандидаты на Z
        std::string function_name;
        uint32_t n_vars;
    };

    // Функции подобраны так, чтобы X->Thr (где встречается) был осмысленным
    // (maj_n11 — пороговая по построению); для остальных пар — обычная MAJ,
    // чтобы можно было честно сравнивать одну и ту же функцию across всех Z.
    std::vector<Case> cases = {
        {Repr::Aig, Repr::Thr, {Repr::Bdd, Repr::Anf, Repr::Tt}, "maj", 11},
        {Repr::Anf, Repr::Bdd, {Repr::Aig, Repr::Tt}, "maj", 11},
        {Repr::Bdd, Repr::Anf, {Repr::Aig, Repr::Tt}, "maj", 11},
        {Repr::Aig, Repr::Anf, {Repr::Bdd, Repr::Tt}, "maj", 13},
        {Repr::Anf, Repr::Aig, {Repr::Bdd, Repr::Tt}, "maj", 13},
        // Повтор самых интересных пар на большем n — проверить, растёт ли
        // разрыв direct/обходной путь с размером входа, или это эффект,
        // заметный только на малых n (накладные расходы). Anf/Bdd/Tt/Thr не
        // упираются в верхнюю границу reference_aig (kMaxReferenceAigVars=14,
        // MUX-дерево экспоненциально) — для них можно уйти выше n=14;
        // случаи, где X или Y == Aig, ограничены этим потолком.
        {Repr::Anf, Repr::Bdd, {Repr::Tt}, "maj", 17},
        {Repr::Bdd, Repr::Anf, {Repr::Tt}, "maj", 17},
        {Repr::Aig, Repr::Thr, {Repr::Bdd, Repr::Anf, Repr::Tt}, "maj", 13},
        {Repr::Aig, Repr::Bdd, {Repr::Anf, Repr::Tt}, "maj", 13},
        {Repr::Bdd, Repr::Aig, {Repr::Anf, Repr::Tt}, "maj", 13},
        // По запросу пользователя ("можно ли обойти лимит через другое
        // представление?") — bdd_to_thr работает НАПРЯМУЮ только для K<=6
        // (CHOW_DATABASE, см. bdd/README.md §5.4а), K>6 -> NotImplemented.
        // Bdd->Tt->Thr обходит эту границу: bdd_to_tt работает для любого
        // n<=kMaxTruthTableVars=24, а tt_to_thr — общий ILP-путь (Muroga),
        // не завязанный на маленькую справочную таблицу. n=7/9/11/13 —
        // нечётные (только там "maj_nX" существует в growing_test_functions,
        // majority определена только для нечётного n), все СТРОГО БОЛЬШЕ
        // K<=6 — прямой путь ожидаемо недоступен на всех четырёх.
        {Repr::Bdd, Repr::Thr, {Repr::Tt}, "maj", 7},
        {Repr::Bdd, Repr::Thr, {Repr::Tt}, "maj", 9},
        {Repr::Bdd, Repr::Thr, {Repr::Tt}, "maj", 11},
        {Repr::Bdd, Repr::Thr, {Repr::Tt}, "maj", 13},
    };

    rep.line("| X | Y | n | прямой X->Y, мс | обход X->Z->Y (лучший), мс | Z | вывод |");
    rep.table_row({"---", "---", "---", "---", "---", "---", "---"});

    for (const auto& c : cases) {
        auto pool = growing_test_functions(std::max(c.n_vars, 3u));
        const GroundTruthFunction* found = nullptr;
        for (const auto& g : pool) {
            if (g.n_vars == c.n_vars && g.name.rfind("maj_", 0) == 0) { found = &g; break; }
        }
        if (!found) continue;

        auto start = build_repr(c.from, *found);
        if (!start) continue;

        auto direct = run_chain(*start, {c.from, c.to});
        double direct_ms = direct.ok ? direct.total_ms : -1.0;

        double best_indirect_ms = -1.0;
        Repr best_z = c.via.empty() ? c.from : c.via.front();
        for (Repr z : c.via) {
            auto indirect = run_chain(*start, {c.from, z, c.to});
            if (!indirect.ok) continue;
            if (best_indirect_ms < 0.0 || indirect.total_ms < best_indirect_ms) {
                best_indirect_ms = indirect.total_ms;
                best_z = z;
            }
        }

        std::string verdict;
        if (direct_ms < 0.0 && best_indirect_ms < 0.0) {
            verdict = "оба пути недоступны на этом входе";
        } else if (direct_ms < 0.0) {
            verdict = "прямой путь недоступен, обходной — единственный рабочий";
        } else if (best_indirect_ms < 0.0) {
            verdict = "обходные пути недоступны, прямой — единственный рабочий";
        } else if (best_indirect_ms < direct_ms) {
            verdict = "обход через " + std::string(repr_name(best_z)) + " БЫСТРЕЕ прямого (" +
                      std::to_string(direct_ms / best_indirect_ms) + "x)";
        } else {
            verdict = "прямой путь быстрее (или сопоставимо)";
        }

        rep.table_row({repr_name(c.from), repr_name(c.to), std::to_string(found->n_vars),
                        direct_ms < 0.0 ? "н/д" : std::to_string(direct_ms),
                        best_indirect_ms < 0.0 ? "н/д" : std::to_string(best_indirect_ms),
                        repr_name(best_z), verdict});
        std::printf("%s->%s (n=%u): прямой=%.3fмс, обход через %s=%.3fмс — %s\n", repr_name(c.from),
                    repr_name(c.to), found->n_vars, direct_ms, repr_name(best_z), best_indirect_ms,
                    verdict.c_str());
    }
}

int g_result = 0;

VOID_TASK_0(chain_tests_main) {
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 22, 1LL << 26);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    Report rep;
    rep.md << "# Отчёт: тесты цепочек трансляций (verify/chain_tests.cpp)\n\n"
              "Автоматически перезаписывается при каждом запуске `test_chains` — не редактировать руками.\n";

    auto t_start = std::chrono::steady_clock::now();

    run_round_trips(rep);
    run_longer_cycles(rep);
    run_indirect_vs_direct(rep);

    auto t_end = std::chrono::steady_clock::now();
    double total_s = std::chrono::duration<double>(t_end - t_start).count();

    rep.md << "\n## Итог\n\n" << rep.total_checks << " проверок, " << rep.failed_checks
           << " провалов. Полное время прогона: " << total_s << " с.\n";

    std::printf("\n=== ИТОГ: %d проверок, %d провалов, %.2f с ===\n", rep.total_checks,
                rep.failed_checks, total_s);

    // Путь относительно рабочей директории процесса — CMakeLists.txt
    // фиксирует WORKING_DIRECTORY=CMAKE_CURRENT_SOURCE_DIR для этого таргета
    // при регистрации через add_test (тот же паттерн, что и у таргета
    // `status`/generate_status.sh), так что "verify/..." всегда указывает на
    // папку в дереве исходников, а не в build/.
    std::ofstream out("verify/CHAIN_TESTS_REPORT.md");
    if (out) {
        out << rep.md.str();
        std::printf("Отчёт записан в verify/CHAIN_TESTS_REPORT.md\n");
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
    RUN(chain_tests_main);
    lace_stop();
    return g_result;
}
