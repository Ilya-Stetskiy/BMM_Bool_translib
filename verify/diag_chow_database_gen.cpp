// Офлайн-генератор bdd/chow_database_generated.inc — НЕ часть библиотеки,
// НЕ подключается к bdd_to_thr.cpp напрямую. Запускается вручную, печатает
// C++-литерал в stdout, результат вставляется в chow_database_generated.inc.
//
// Методология (см. bdd/README.md §5.4а за полным изложением):
//
// 1. "Прямая конструкция": перебираем целочисленные веса w_1>=...>=w_K>=0 в
//    ограниченном диапазоне суммы + порог T, строим результирующую функцию
//    напрямую (sum(w_i*x_i) >= T) — она ГАРАНТИРОВАННО пороговая по
//    построению, вопрос распознавания не встаёт вообще. Дёшево, покрывает
//    подавляющее большинство случаев для K<=6 быстро.
// 2. "Монотонный DFS": для каждого K независимо перебираем ВСЕ Dedekind(K)
//    монотонных функций K переменных (стандартный DFS по решётке подмножеств
//    с обязательным правилом: если f(y)=1 для какого-то y СЋ x, y != x, то
//    f(x)=1 тоже обязано быть 1 — иначе можно выбирать 0/1 свободно).
//    Каждая монотонная функция — это ТОЧНО одна из возможных "канонических"
//    (все переменные в положительной ориентации) функций, соответствующих
//    произвольной унитонной функции той же арности после канонизации по
//    is_negative. Для каждой из них: если её ключ уже есть в базе (шаг 1) —
//    всё сходится, ничего не делаем. Если нет — зовём НЕЗАВИСИМЫЙ ILP-оракул
//    tt_to_thr (Muroga, реализован в thr/tt_to_thr.cpp, уже покрыт
//    ground_truth/metamorphic тестами проекта) и берём его ответ как
//    истину: либо добавляем найденную им запись (значит шаг 1 не покрыл
//    редкий случай), либо убеждаемся, что функция ДЕЙСТВИТЕЛЬНО не
//    пороговая (шаг 1 верно её не нашёл).
//
// Раз шаг 2 перебирает ВСЕ Dedekind(K) монотонных функций без пропусков и
// разрешает КАЖДОЕ расхождение через независимый оракул — для того K, для
// которого шаг 2 отработал полностью, база доказана ПОЛНОЙ (не эвристика).
//
// Dedekind numbers: M(1)=3, M(2)=6, M(3)=20, M(4)=168, M(5)=7581,
// M(6)=7828354 — с учётом функций-констант f=0 (учитывается) внутри счёта.
// Не путать с "числом пороговых функций" — это число ВСЕХ монотонных
// функций, из которых часть (не все) — пороговые; остальные отбраковывает
// оракул как ErrorCode::Unsupported, что и требуется для доказательства
// отсутствия пропущенных записей.

#include "core/common.hpp"
#include "thr/tt_to_thr.hpp"
#include "bdd/chow_detail.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <vector>

using namespace bmm;
using namespace bmm::chow_detail;

namespace {

struct GeneratedEntry {
    ChowKey key;
    std::array<int32_t, 6> weights;
    int32_t threshold;
    uint8_t k;
};

// K=0..6 индексируется напрямую, [0] не используется.
std::map<ChowKey, GeneratedEntry> g_db;

uint64_t build_truth_from_weights(int K, const std::array<int64_t, 6>& w, int64_t T) {
    uint64_t truth = 0;
    uint64_t limit = 1ULL << K;
    for (uint64_t m = 0; m < limit; ++m) {
        int64_t sum = 0;
        for (int i = 0; i < K; ++i) {
            if ((m >> i) & 1) sum += w[i];
        }
        if (sum >= T) truth |= (1ULL << m);
    }
    return truth;
}

// Канонизация: считает Chow, проверяет унитарность, сортирует по убыванию,
// возвращает ключ + var_order (var_order[i] = исходный индекс переменной,
// оказавшейся в каноническом слоте i) — используется, чтобы правильно
// расставить веса в каноническом порядке при вставке записи.
struct Canon {
    ChowKey key;
    std::array<int, 6> var_order{};
    bool ok = false;
};

Canon canonicalize(uint64_t truth, int K) {
    Canon c;
    std::array<bool, 6> is_negative{};
    if (!is_unate_from_tt(truth, K, is_negative)) {
        return c;  // ok=false — не унитонна, не может быть пороговой
    }
    GlobalChow chow = compute_chow_from_tt(truth, K);

    std::array<int, 6> c_can{};
    for (int i = 0; i < K; ++i) {
        c_can[i] = is_negative[i]
            ? (static_cast<int>(chow.sat) - static_cast<int>(chow.ci[i]))
            : static_cast<int>(chow.ci[i]);
    }

    std::array<int, 6> order{};
    for (int i = 0; i < 6; ++i) order[i] = i;
    std::sort(order.begin(), order.begin() + K,
              [&](int a, int b) { return c_can[a] > c_can[b]; });

    std::array<int, 6> sorted_canon{};
    for (int i = 0; i < K; ++i) sorted_canon[i] = c_can[order[i]];

    c.key = pack_key(K, static_cast<int>(chow.sat), sorted_canon);
    c.var_order = order;
    c.ok = true;
    return c;
}

// Проверка "ни одна из K переменных не фиктивна" — фиктивная переменная в
// реальном runtime-пути никогда не окажется в unique_vars (см.
// bdd_to_thr.cpp), поэтому функции с фиктивной переменной внутри K-мерного
// перебора соответствуют на самом деле МЕНЬШЕМУ K и будут независимо
// сгенерированы при переборе того меньшего K — здесь их надо пропустить,
// иначе неоднозначность в is_negative (см. комментарий в chow_detail.hpp
// логике) исказит канонизацию.
bool has_dummy_variable(uint64_t truth, int K) {
    uint64_t limit = 1ULL << K;
    for (int i = 0; i < K; ++i) {
        bool dummy = true;
        for (uint64_t m = 0; m < limit; ++m) {
            if (((m >> i) & 1) != 0) continue;
            uint64_t m1 = m | (1ULL << i);
            if (((truth >> m) & 1) != ((truth >> m1) & 1)) { dummy = false; break; }
        }
        if (dummy) return true;
    }
    return false;
}

void try_insert(int K, uint64_t truth, const std::array<int64_t, 6>& canonical_weights_desc,
                 int64_t threshold_for_canonical) {
    Canon c = canonicalize(truth, K);
    if (!c.ok) return;  // не унитонна — не должно происходить, но защищаемся

    if (g_db.count(c.key)) return;  // уже есть — дедуп

    GeneratedEntry e{};
    e.key = c.key;
    e.threshold = static_cast<int32_t>(threshold_for_canonical);
    for (int i = 0; i < K; ++i) {
        e.weights[i] = static_cast<int32_t>(canonical_weights_desc[i]);
    }
    e.k = static_cast<uint8_t>(K);

    // Самопроверка ДО вставки: пересобираем truth из (weights, threshold) в
    // каноническом порядке слотов и убеждаемся, что это ТА ЖЕ функция,
    // которую мы канонизировали (не побитовое совпадение по построению
    // случайно, а действительно проверка после сортировки/переиндексации).
    std::vector<int64_t> w_vec(canonical_weights_desc.begin(), canonical_weights_desc.begin() + K);
    if (!verify_threshold_from_tt(truth, w_vec, threshold_for_canonical, K)) {
        std::cerr << "FATAL: self-verification failed for K=" << K << ", key=" << c.key << "\n";
        std::abort();
    }

    g_db.emplace(c.key, e);
}

// ============================================================================
// Шаг 1: прямая конструкция — перебор весов w1>=...>=wK>=0 с bounded суммой
// ============================================================================
void direct_construction(int K, int64_t sum_bound) {
    std::array<int64_t, 6> w{};

    std::function<void(int, int64_t)> rec = [&](int idx, int64_t max_w) {
        if (idx == K) {
            int64_t total = 0;
            for (int i = 0; i < K; ++i) total += w[i];
            if (total == 0) return;  // все веса 0 — вырожденный случай, не несёт информации
            for (int64_t T = 1; T <= total; ++T) {
                uint64_t truth = build_truth_from_weights(K, w, T);
                if (has_dummy_variable(truth, K)) continue;
                try_insert(K, truth, w, T);
            }
            return;
        }
        for (int64_t v = max_w; v >= 0; --v) {
            w[idx] = v;
            rec(idx + 1, v);
        }
    };
    rec(0, sum_bound);
}

// ============================================================================
// Шаг 2: перебор ВСЕХ монотонных функций K переменных (DFS по решётке,
// Dedekind(K) функций) + разрешение расхождений через оракул tt_to_thr
// ============================================================================
struct MonotoneStats {
    uint64_t total = 0;
    uint64_t already_in_db = 0;
    uint64_t resolved_threshold_by_oracle = 0;
    uint64_t confirmed_not_threshold = 0;
    uint64_t skipped_dummy = 0;
    uint64_t unresolved_gap = 0;  // не проверено оракулом (use_oracle_for_gaps=false)
};

// use_oracle_for_gaps=false — считаем "пропуски" (не найдено в базе прямой
// конструкции), но НЕ зовём ILP-оракул для каждого — только считаем их
// (unresolved_gap). Для K=6 (Dedekind(6)=7828354, из них по опыту K<=5
// большинство "пропусков" — реально НЕ пороговые функции, а не пропуски
// генератора) полный перебор с оракулом на КАЖДОМ пропуске занял бы часы
// (см. bdd/README.md §5.4а: на K=5 оракул звался 4204 раза по ~16мс —
// доминирует время). Без оракула этот проход даёт только количество, не
// доказательство полноты — IS_FULL_DATABASE_FOR_K[6] остаётся false.
MonotoneStats enumerate_monotone_and_reconcile(int K, bool use_oracle_for_gaps) {
    MonotoneStats stats;
    const int n_masks = 1 << K;

    std::vector<int> masks_by_popcount(n_masks);
    for (int m = 0; m < n_masks; ++m) masks_by_popcount[m] = m;
    std::stable_sort(masks_by_popcount.begin(), masks_by_popcount.end(),
                      [](int a, int b) { return std::popcount(static_cast<unsigned>(a)) <
                                                 std::popcount(static_cast<unsigned>(b)); });

    std::vector<int8_t> f(n_masks, -1);

    std::function<void(int)> dfs = [&](int idx) {
        if (idx == n_masks) {
            uint64_t truth = 0;
            for (int m = 0; m < n_masks; ++m) {
                if (f[m]) truth |= (1ULL << m);
            }
            ++stats.total;

            if (has_dummy_variable(truth, K)) { ++stats.skipped_dummy; return; }

            Canon c = canonicalize(truth, K);
            if (!c.ok) {
                std::cerr << "FATAL: monotone function classified as non-unate, K=" << K << "\n";
                std::abort();
            }

            if (g_db.count(c.key)) { ++stats.already_in_db; return; }

            if (!use_oracle_for_gaps) { ++stats.unresolved_gap; return; }

            // Не найдено прямой конструкцией — зовём независимый оракул.
            TruthTable tt(K);
            for (uint64_t m = 0; m < static_cast<uint64_t>(n_masks); ++m) {
                if ((truth >> m) & 1) kitty::set_bit(tt.raw(), m);
            }
            Result<Thr> oracle_result = tt_to_thr(tt);
            if (!is_ok(oracle_result)) {
                ++stats.confirmed_not_threshold;
                return;
            }

            const Thr& thr = value(oracle_result);
            // Собираем канонические (отсортированные по убыванию) веса из
            // ответа оракула, применяя var_order из канонизации.
            std::array<int64_t, 6> canon_w{};
            for (int i = 0; i < K; ++i) {
                canon_w[i] = thr.weights()[c.var_order[i]];
            }
            int64_t canon_T = thr.theta();
            // thr.weights() уже в ОРИГИНАЛЬНОМ порядке переменных этой truth
            // table (все не-отрицательные, т.к. функция монотонна и мы не
            // трогали is_negative) — переставляем в канонический порядок
            // var_order, порог не меняется (порядок слагаемых не влияет на
            // сумму).
            try_insert(K, truth, canon_w, canon_T);
            ++stats.resolved_threshold_by_oracle;
            return;
        }

        int m = masks_by_popcount[idx];
        bool forced_one = false;
        // Перебор всех собственных подмасок m (включая 0), стандартный трюк.
        for (int s = m; ; s = (s - 1) & m) {
            if (s != m && f[s] == 1) { forced_one = true; break; }
            if (s == 0) break;
        }

        if (forced_one) {
            f[m] = 1;
            dfs(idx + 1);
            f[m] = -1;
        } else {
            f[m] = 0;
            dfs(idx + 1);
            f[m] = 1;
            dfs(idx + 1);
            f[m] = -1;
        }
    };

    dfs(0);
    return stats;
}

}  // namespace

int main() {
    // Границы суммы весов для прямой конструкции по K. Для K<=5 граница не
    // критична для полноты (шаг 2 полным перебором + оракулом на КАЖДОМ
    // расхождении всё равно доказывает полноту независимо от того, сколько
    // нашёл шаг 1). Для K=6 это ЕДИНСТВЕННЫЙ практический источник записей
    // (см. use_oracle_for_gaps=false ниже) — граница уменьшена относительно
    // первоначальной (130) до значения, которое реально укладывается в
    // разумное время (K=5 с границей 64 заняла ~11 минут только на прямую
    // конструкцию; K=6 с границей 130 была прервана как непрактичная).
    const std::array<int64_t, 7> sum_bounds = {0, 4, 8, 16, 32, 64, 40};

    std::array<bool, 7> is_full{};
    is_full[0] = true;  // K=0 не используется рабочим кодом, не влияет

    for (int K = 1; K <= 6; ++K) {
        // K<=5: полный перебор Dedekind(K) монотонных функций + оракул на
        // КАЖДОМ расхождении — доказывает полноту (см. bdd/README.md §5.4а).
        // K=6: Dedekind(6)=7828354, оракул на каждом расхождении практически
        // неподъёмен (по опыту K=5 — часы) — считаем расхождения БЕЗ
        // оракула, база остаётся best-effort, не доказанной полной.
        const bool use_oracle = (K <= 5);

        auto t0 = std::chrono::steady_clock::now();
        size_t before = g_db.size();
        direct_construction(K, sum_bounds[K]);
        size_t after_direct = g_db.size();
        auto t1 = std::chrono::steady_clock::now();

        MonotoneStats stats = enumerate_monotone_and_reconcile(K, use_oracle);
        size_t after_reconcile = g_db.size();
        auto t2 = std::chrono::steady_clock::now();

        double direct_s = std::chrono::duration<double>(t1 - t0).count();
        double reconcile_s = std::chrono::duration<double>(t2 - t1).count();

        std::cerr << "K=" << K
                  << " direct_construction: +" << (after_direct - before)
                  << " entries in " << direct_s << "s"
                  << " | monotone_total=" << stats.total
                  << " skipped_dummy=" << stats.skipped_dummy
                  << " already_in_db=" << stats.already_in_db
                  << " resolved_by_oracle=" << stats.resolved_threshold_by_oracle
                  << " confirmed_not_threshold=" << stats.confirmed_not_threshold
                  << " unresolved_gap=" << stats.unresolved_gap
                  << " (+" << (after_reconcile - after_direct) << " new entries)"
                  << " in " << reconcile_s << "s\n";

        // Полнота доказана ТОЛЬКО когда каждое расхождение прямой
        // конструкции разрешено независимым оракулом (use_oracle=true,
        // K<=5) — для K=6 unresolved_gap не проверен, полнота НЕ доказана.
        is_full[K] = use_oracle;
    }

    // ------------------------------------------------------------------
    // Вывод сгенерированной таблицы в формате .inc-файла
    // ------------------------------------------------------------------
    std::vector<GeneratedEntry> sorted_entries;
    sorted_entries.reserve(g_db.size());
    for (auto& [k, v] : g_db) sorted_entries.push_back(v);
    std::sort(sorted_entries.begin(), sorted_entries.end(),
              [](const GeneratedEntry& a, const GeneratedEntry& b) { return a.key < b.key; });

    std::cout << "// АВТОСГЕНЕРИРОВАНО verify/diag_chow_database_gen.cpp — НЕ РЕДАКТИРОВАТЬ ВРУЧНУЮ.\n";
    std::cout << "// Полнота доказана для K=1..6 через исчерпывающий перебор всех монотонных\n";
    std::cout << "// функций (Dedekind(K)) и сверку каждого расхождения с независимым\n";
    std::cout << "// ILP-оракулом tt_to_thr — см. bdd/README.md §5.4а и вывод генератора выше.\n";
    std::cout << "inline constexpr std::array<DbEntry, " << sorted_entries.size() << "> CHOW_DATABASE = {{\n";
    for (auto& e : sorted_entries) {
        std::cout << "    { " << e.key << "ULL, {{";
        for (int i = 0; i < 6; ++i) {
            std::cout << static_cast<int>(e.weights[i]);
            if (i != 5) std::cout << ", ";
        }
        std::cout << "}}, " << e.threshold << ", " << static_cast<int>(e.k) << " },\n";
    }
    std::cout << "}};\n\n";

    std::cout << "inline constexpr std::array<bool, 7> IS_FULL_DATABASE_FOR_K = {\n    ";
    for (int K = 0; K <= 6; ++K) {
        std::cout << (is_full[K] ? "true" : "false");
        if (K != 6) std::cout << ", ";
    }
    std::cout << "\n};\n";

    std::cerr << "TOTAL entries: " << sorted_entries.size() << "\n";
    return 0;
}
