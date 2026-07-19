// benchmarks/dddmp_loader.hpp — мост DDDMP-2.0 (persons.iis.nsk.su,
// benchmarks/data/iis-nsk/bdd-bench/*.bdd и встроенные .bdd/.fdd/.zdd в
// ANF-архивах 100-10k-rnd) в sylvan::Bdd (используемый проектом формат BDD,
// core/CONVENTIONS.md п.6).
//
// DDDMP — формат CUDD (Cabodi/Quer, Politecnico di Torino), НЕ формат
// Sylvan напрямую. Единственный практичный путь получить из него
// sylvan::Bdd — загрузить через СОБСТВЕННЫЙ загрузчик CUDD
// (Dddmp_cuddBddArrayLoad, встроен в libcudd.a этого образа, собран как
// зависимость BRiAl с --enable-dddmp) в DdNode*, а затем рекурсивно
// перестроить эквивалентный sylvan::Bdd через Cudd_T/Cudd_E/
// Cudd_IsComplement + Ite() — тот же общий Shannon-разложение подход, что
// используют bdd_to_* функции проекта для СОБСТВЕННОГО обхода Bdd, только
// источник узлов — CUDD, а не Sylvan.
//
// ВАЖНО про порядок переменных (найдено и исправлено этой сессией —
// НЕ теоретический риск, реально воспроизведено на реальном файле
// Cookbook__bilbo_lfsr.blif.bdd, 201 узел/67 переменных — казалось бы,
// тривиальный размер, но первая версия моста висела на нём): физический
// уровень Sylvan для sylvan::Bdd::bddVar(i) — это ВСЕГДА индекс i (см.
// анаЛОГичное рассуждение в aig_to_bdd.cpp/anf_to_bdd.cpp про
// var_to_level). CUDD, напротив, хранит переменные под СОБСТВЕННЫМ,
// обычно неидентичным порядком уровней (динамическое переупорядочивание
// внутри CUDD, .permids в заголовке DDDMP-файла — не идентичная
// перестановка почти никогда). Компактность ИСХОДНОГО CUDD BDD (201 узел)
// достигнута именно за счёт ЭТОГО, CUDD-собственного порядка — если
// реконструировать BDD в Sylvan, тупо используя bddVar(ЛОГИЧЕСКИЙ_id)
// (что эквивалентно ИДЕНТИЧНОМУ порядку в Sylvan), результат может быть
// экспоненциально хуже (тот же класс явления, что задокументирован для
// anf_to_bdd/aig_to_bdd без эвристики — натуральный/неудачный порядок
// переменных может быть кратно/экспоненциально хуже оптимального).
// Фикс — Cudd_ReadPerm(mgr, var) даёт РЕАЛЬНЫЙ текущий уровень переменной
// var в САМОМ CUDD; используем его как sylvan-уровень (bddVar(rank[var])),
// сохраняя тот порядок, который и сделал исходный CUDD BDD компактным, и
// возвращаем итоговый Bdd с явным var_to_level (Bdd(root, n_vars,
// var_to_level), core/common.hpp) — тот же паттерн, что уже применяют
// anf_to_bdd_with_heuristic/aig_to_bdd_with_heuristic для собственного
// FORCE-порядка.
//
// ВАЖНО (отдельно): этот заголовок специально ОТДЕЛЁН от core/anf_repr.hpp
// (BRiAl) — BRiAl использует CUDD ВНУТРИ себя через собственные bundled
// заголовки (polybori/cudd/cudd.h), а не через системные
// /usr/local/include/cudd.h+dddmp.h, которые нужны здесь для
// Dddmp_*-символов (BRiAl их не экспортирует). Смешивание обоих path'ов
// cudd.h в одной единице трансляции даёт ошибки компиляции (конфликтующие
// объявления одних и тех же имён функций с разными типами менеджера) —
// проверено эмпирически при попытке объединить с verify/chain_utils.hpp
// (тянет core/anf_repr.hpp) в одном .cpp. Не включайте core/anf_repr.hpp
// туда же, где включён этот файл — если нужно сравнить Anf и загруженный
// DDDMP Bdd, обменивайтесь данными через промежуточный файл между двумя
// отдельными бинарями (см. verify/diag_anf_sample_dump.cpp +
// verify/diag_dddmp_sweep.cpp за примером).
//
// dddmp.h требует "util.h" (наследие пакетной структуры исходного дерева
// CUDD) — в этом образе его нет (не установлен, только cudd.h/dddmp.h);
// проверено эмпирически, что dddmp.h/cudd.h не используют из него ни
// одного идентификатора напрямую, поэтому пустая заглушка
// (benchmarks/cudd_stubs/util.h, добавлена в include path ПЕРЕД системным
// в CMakeLists.txt) достаточна.

#pragma once

#include "core/common.hpp"

#include <cstdio>
#include <cstdlib>

extern "C" {
#include <cudd.h>
#include <dddmp.h>
}

#include <sylvan_obj.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bmm::benchmarks {

struct DddmpBdds {
    std::vector<sylvan::Bdd> roots;      // все корни файла, в порядке .rootids
    uint32_t n_vars = 0;                 // Cudd_ReadSize(mgr) — общее число переменных
    std::vector<uint32_t> var_to_level;  // var_to_level[var] = Cudd_ReadPerm(mgr, var)
};

namespace detail {

// Рекурсивный перевод одного узла CUDD DdNode* в sylvan::Bdd. Мемоизация по
// РЕГУЛЯРНОМУ (без бита дополнения, см. Cudd_Regular) указателю узла — один
// и тот же CUDD-узел обычно достижим из многих путей (общие поддеревья DAG,
// то же соображение, что и в bdd_to_anf.cpp/anf_to_bdd.cpp Memo) — без
// мемоизации перевод был бы экспоненциальным по количеству путей вместо
// линейного по числу узлов.
//
// Единственный константный узел CUDD (по конструкции CUDD BDD — Cudd_T/
// Cudd_E никогда не встречают "0" отдельным узлом, только "1" с битом
// дополнения на ссылающемся ребре, см. Cudd_IsComplement) переводится в
// sylvan::Bdd::bddOne(); бит дополнения на исходной ссылке применяется
// снаружи (тот же оператор `!`, что использует aig_to_bdd.cpp/
// anf_to_bdd.cpp для инвертированных сигналов Sylvan).
//
// var_to_level[Cudd_NodeReadIndex(reg)] — level = РЕАЛЬНЫЙ уровень
// переменной в исходном CUDD BDD (Cudd_ReadPerm), не голый логический
// индекс — см. подробное обоснование в шапке файла (без этого — риск
// экспоненциального взрыва результата, воспроизведено эмпирически).
inline sylvan::Bdd cudd_to_sylvan(DdNode* node, const std::vector<uint32_t>& var_to_level,
                                   std::unordered_map<DdNode*, sylvan::Bdd>& memo) {
    DdNode* reg = Cudd_Regular(node);
    const bool comp = Cudd_IsComplement(node) != 0;

    auto it = memo.find(reg);
    if (it != memo.end()) {
        return comp ? !it->second : it->second;
    }

    sylvan::Bdd result;
    if (Cudd_IsConstant(reg)) {
        result = sylvan::Bdd::bddOne();
    } else {
        const uint32_t var = Cudd_NodeReadIndex(reg);
        sylvan::Bdd then_bdd = cudd_to_sylvan(Cudd_T(reg), var_to_level, memo);
        sylvan::Bdd else_bdd = cudd_to_sylvan(Cudd_E(reg), var_to_level, memo);
        result = sylvan::Bdd::bddVar(var_to_level[var]).Ite(then_bdd, else_bdd);
    }
    memo.emplace(reg, result);
    return comp ? !result : result;
}

}  // namespace detail

// Загружает ВСЕ корни .bdd/.fdd/.zdd-файла (DDDMP-2.0). ДОЛЖНА вызываться
// внутри Lace-задачи (тот же контракт, что и весь код, трогающий
// sylvan::Bdd, — см. verify/test_main.cpp): sylvan::Bdd::Ite/bddVar
// требуют активного, уже инициализированного Sylvan-контекста внутри
// зарегистрированного Lace-воркера.
//
// DDDMP_ROOT_MATCHLIST (не DDDMP_ROOT_MATCHNAMES) — эмпирически
// подтверждено: MATCHNAMES безусловно ожидает валидный массив имён корней
// для сопоставления (сегфолт при nullptr), MATCHLIST — "взять все корни
// файла как есть", ровно то, что нужно (не пытаемся выбрать подмножество
// по имени). DDDMP_VAR_MATCHIDS — переменные создаются/сопоставляются по
// численным id файла (.ids), не по именам/permids — соответствует тому,
// как остальной проект оперирует переменными (0-индексированные номера,
// core/CONVENTIONS.md); порядок УРОВНЕЙ (не идентичности переменных)
// сохраняется отдельно через var_to_level (Cudd_ReadPerm), см. шапку файла.
inline std::optional<DddmpBdds> load_dddmp_bdds(const std::string& path) {
    DdManager* mgr = Cudd_Init(0, 0, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    if (!mgr) return std::nullopt;

    DdNode** roots = nullptr;
    const int n_roots = Dddmp_cuddBddArrayLoad(
        mgr, DDDMP_ROOT_MATCHLIST, nullptr, DDDMP_VAR_MATCHIDS, nullptr, nullptr, nullptr,
        DDDMP_MODE_DEFAULT, const_cast<char*>(path.c_str()), nullptr, &roots);

    if (n_roots <= 0 || !roots) {
        Cudd_Quit(mgr);
        return std::nullopt;
    }

    DddmpBdds result;
    result.n_vars = static_cast<uint32_t>(Cudd_ReadSize(mgr));
    result.var_to_level.resize(result.n_vars);
    for (uint32_t v = 0; v < result.n_vars; ++v) {
        result.var_to_level[v] = static_cast<uint32_t>(Cudd_ReadPerm(mgr, static_cast<int>(v)));
    }

    std::unordered_map<DdNode*, sylvan::Bdd> memo;
    result.roots.reserve(static_cast<size_t>(n_roots));
    for (int i = 0; i < n_roots; ++i) {
        result.roots.push_back(detail::cudd_to_sylvan(roots[i], result.var_to_level, memo));
    }

    for (int i = 0; i < n_roots; ++i) Cudd_RecursiveDeref(mgr, roots[i]);
    std::free(roots);
    Cudd_Quit(mgr);

    return result;
}

// Один конкретный корень как bmm::Bdd (core/common.hpp) — удобная обёртка
// для случая "один выход интересен" (например, сверка с конкретным PO
// реальной многовыходной схемы, см. verify/diag_dddmp_dataset.cpp).
// Bdd(root, n_vars, var_to_level) — явный порядок уровней (см. шапку
// файла), НЕ Bdd(root, n_vars) (тот означал бы identity — тот самый
// источник экспоненциального взрыва, который здесь исправлен).
inline std::optional<Bdd> load_dddmp_bdd(const std::string& path, size_t root_index) {
    auto all = load_dddmp_bdds(path);
    if (!all || root_index >= all->roots.size()) return std::nullopt;
    return Bdd(all->roots[root_index], all->n_vars, all->var_to_level);
}

}  // namespace bmm::benchmarks
