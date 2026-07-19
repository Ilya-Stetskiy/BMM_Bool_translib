// Временный диагностический бинарь — два дополнительных прогона моста
// benchmarks/dddmp_loader.hpp:
//   1) "load"  — тихий прогон по всем 895 файлам benchmarks/data/iis-nsk/
//      bdd-bench/*.bdd (только успешная загрузка + разумные n_vars/
//      NodeCount, без сравнения с чем-либо — общая проверка робастности
//      моста на РАЗНООБРАЗНЫХ реальных DDDMP-файлах, не только 4 совпавших
//      с EPFL из verify/diag_dddmp_dataset.cpp).
//   2) "anf <samples_path>" — сверка ВСТРОЕННОГО эталона (.anf.bdd.bdd) в
//      реальном ANF-корпусе n=100/M=10000 против точек, посчитанных
//      Anf::evaluate() ЗАРАНЕЕ отдельным процессом (verify/
//      diag_anf_sample_dump.cpp, см. объяснение разделения на два бинаря
//      в шапке benchmarks/dddmp_loader.hpp — конфликт cudd.h) — валидирует,
//      что представление Anf в проекте корректно ДАЖЕ там, где наш
//      собственный anf_to_bdd практически не может посчитать ответ (см.
//      SESSION_REPORT.md §9.2) — эталон здесь сторонний (посчитан не
//      библиотекой), а не anf_to_bdd().

#include "benchmarks/dddmp_loader.hpp"

#include <sylvan_obj.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace bmm;
using namespace bmm::benchmarks;
using namespace sylvan;  // нужно для unqualified sylvan_gc() (VOID_TASK_DECL_0 внутри namespace sylvan)

namespace {

void sweep_load(int& total, int& ok, int& fail, const std::string& single_file) {
    for (const auto& entry :
         std::filesystem::directory_iterator("benchmarks/data/iis-nsk/bdd-bench")) {
        if (entry.path().extension() != ".bdd") continue;
        if (!single_file.empty() && entry.path().filename().string() != single_file) continue;
        ++total;
        std::printf(">>> %s\n", entry.path().string().c_str());
        std::fflush(stdout);
        auto loaded = load_dddmp_bdds(entry.path().string());
        if (!loaded || loaded->roots.empty()) {
            std::printf("FAIL %s\n", entry.path().string().c_str());
            std::fflush(stdout);
            ++fail;
            continue;
        }
        ++ok;
        // Явный sweep_load-специфичный gc: без него узлы КАЖДОГО
        // предыдущего файла (у Cookbook__arbiter.blif.bdd — сотни тысяч)
        // остаются в общей Sylvan-таблице между файлами одного процесса —
        // эмпирически подтверждено, что без явного gc() последующие файлы
        // (даже крошечные) резко замедляются/зависают. Не относится к
        // самому мосту (benchmarks/dddmp_loader.hpp) — там каждый вызов
        // независим; это гигиена именно МНОГОФАЙЛОВОГО прогона в одном
        // процессе.
        loaded.reset();
        sylvan_gc();
        if (total % 20 == 0) {
            std::printf("... %d файлов обработано (ok=%d fail=%d)\n", total, ok, fail);
            std::fflush(stdout);
        }
    }
}

void anf_cross_check(const std::string& samples_path, const std::string& bdd_path) {
    std::ifstream in(samples_path);
    if (!in) {
        std::printf("FAIL: не удалось открыть %s\n", samples_path.c_str());
        return;
    }
    uint32_t n_vars = 0;
    in >> n_vars;

    auto bdd = load_dddmp_bdd(bdd_path, 0);
    if (!bdd) {
        std::printf("FAIL: не удалось загрузить %s\n", bdd_path.c_str());
        return;
    }
    std::printf("PASS загрузка: Anf n_vars=%u (из файла сэмплов), Bdd n_vars=%u, NodeCount=%zu\n",
                n_vars, bdd->n_vars(), static_cast<size_t>(bdd->raw().NodeCount()));
    std::fflush(stdout);

    if (n_vars != bdd->n_vars()) {
        std::printf("FAIL: n_vars не совпадает\n");
        return;
    }

    std::string bits;
    int expected = 0;
    int n_checked = 0;
    bool all_match = true;
    Assignment assignment(n_vars, false);
    while (in >> bits >> expected) {
        for (uint32_t i = 0; i < n_vars; ++i) assignment[i] = bits[i] == '1';
        bool actual = bdd->evaluate(assignment);
        if (actual != (expected != 0)) {
            all_match = false;
            break;
        }
        ++n_checked;
    }
    std::printf("%s Anf::evaluate() (из файла сэмплов) против стороннего DDDMP-эталона (%d точек)\n",
                all_match ? "PASS" : "FAIL", n_checked);
}

int g_result = 0;
bool g_load_sweep_mode = false;
std::string g_samples_path;
std::string g_bdd_path = "benchmarks/data/iis-nsk/100-10k-rnd/10-100x90-100.01.anf.bdd.bdd";
std::string g_single_file;

// Оба режима трогают sylvan::Bdd (load_dddmp_bdds строит sylvan::Bdd через
// Ite() внутри cudd_to_sylvan) — ОБА обязаны выполняться внутри
// зарегистрированного Lace-воркера (см. verify/test_main.cpp), иначе
// SIGSEGV в Sylvan (llmsset_lookup), не просто в одном из двух режимов.
VOID_TASK_0(main_task) {
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 18, 1LL << 22);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    if (g_load_sweep_mode) {
        int total = 0, ok = 0, fail = 0;
        sweep_load(total, ok, fail, g_single_file);
        std::printf("=== ИТОГ load: %d файлов, %d ok, %d fail ===\n", total, ok, fail);
        g_result = fail == 0 ? 0 : 1;
    } else {
        std::printf("=== anf: встроенный DDDMP-эталон vs Anf::evaluate() ===\n");
        anf_cross_check(g_samples_path, g_bdd_path);
    }

    sylvan::sylvan_quit();
}

}  // namespace

int main(int argc, char** argv) {
    g_load_sweep_mode = argc > 1 && std::string(argv[1]) == "load";
    if (g_load_sweep_mode && argc > 2) g_single_file = argv[2];
    if (!g_load_sweep_mode && argc > 1) g_samples_path = argv[1];

    const size_t deque_size = 1ULL << 21;
    lace_start(0, deque_size);
    RUN(main_task);
    lace_stop();
    return g_result;
}
