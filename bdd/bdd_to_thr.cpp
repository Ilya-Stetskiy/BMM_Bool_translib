#include "bdd_to_thr.hpp"

#include <tracy/Tracy.hpp>
#include <sylvan_obj.hpp>
#include <array>
#include <cstdint>
#include <algorithm>
#include <numeric>
#include <bit>
#include <stdexcept>
#include <vector>
#include <cassert>

namespace bmm {

namespace detail {

enum class ThresholdStatus {
    Threshold,
    NotThreshold,
    NeedFallback
};

template <int K>
struct FastThresholdResult {
    std::array<int32_t, K> weights;
    int32_t threshold;
    ThresholdStatus status;
};

constexpr uint64_t COMP_BIT = 0x8000000000000000ULL;

// ============================================================================
// 1. Сбор Truth Table (Единый источник истины для K ≤ 6)
// ============================================================================
// Для фиксированного ограничения K ≤ 6 truth table является самым простым
// и наиболее надёжным представлением. Несмотря на худшую асимптотику
// O(2^K * height), её абсолютная стоимость настолько мала (не более 64 оценок
// функции), что выигрыш от DP не имеет практического значения, тогда как
// риск логических ошибок в DP существенно выше.
uint64_t build_truth_table(
    const sylvan::Bdd& f,
    int K,
    const std::array<uint32_t, 6>& unique_vars
) {
    uint64_t truth = 0;
    uint64_t limit = 1ULL << K;

    for (uint64_t m = 0; m < limit; ++m) {
        sylvan::Bdd curr = f;
        while (!curr.isTerminal()) {
            uint32_t bv = curr.TopVar();
            int dv = 0;
            while (dv < K && unique_vars[dv] != bv) ++dv;

            if (dv == K) {
                throw std::logic_error("build_truth_table: variable not found in unique_vars");
            }

            curr = ((m >> dv) & 1) ? curr.Then() : curr.Else();
        }
        if (curr.isOne()) {
            truth |= (1ULL << m);
        }
    }
    return truth;
}

// ============================================================================
// 2. Вычисление Chow напрямую из Truth Table
// ============================================================================
struct GlobalChow {
    uint32_t sat;
    std::array<uint32_t, 6> ci;
};

GlobalChow compute_chow_from_tt(uint64_t truth, int K) {
    GlobalChow chow;
    chow.sat = static_cast<uint32_t>(std::popcount(truth));
    chow.ci.fill(0);

    uint64_t limit = 1ULL << K;
    for (uint64_t m = 0; m < limit; ++m) {
        if ((truth >> m) & 1) {
            for (int i = 0; i < K; ++i) {
                if ((m >> i) & 1) {
                    ++chow.ci[i];
                }
            }
        }
    }
    return chow;
}

// ============================================================================
// 3. Проверка унитарности по Truth Table
// ============================================================================
bool is_unate_from_tt(uint64_t truth, int K, std::array<bool, 6>& is_negative) {
    uint64_t limit = 1ULL << K;

    for (int i = 0; i < K; ++i) {
        bool pos = true, neg = true;
        for (uint64_t m = 0; m < limit; ++m) {
            if (((m >> i) & 1) == 0) continue;

            uint64_t m0 = m & ~(1ULL << i);
            uint64_t m1 = m0 | (1ULL << i);

            bool v0 = (truth >> m0) & 1;
            bool v1 = (truth >> m1) & 1;

            if (v0 && !v1) pos = false;
            if (v1 && !v0) neg = false;
            if (!pos && !neg) return false;
        }
        is_negative[i] = (neg && !pos);
    }
    return true;
}

// ============================================================================
// 4. Финальная верификация (ГАРАНТИЯ ОТСУТСТВИЯ FALSE POSITIVES)
// ============================================================================
bool verify_threshold_from_tt(
    uint64_t truth,
    const std::vector<int64_t>& weights,
    int64_t threshold,
    int K
) {
    uint64_t limit = 1ULL << K;
    for (uint64_t m = 0; m < limit; ++m) {
        bool bdd_val = (truth >> m) & 1;
        int64_t sum = 0;
        for (int i = 0; i < K; ++i) {
            if ((m >> i) & 1) sum += weights[i];
        }
        if (bdd_val != (sum >= threshold)) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// 5. База данных и Lookup
// ============================================================================
using ChowKey = uint64_t;

struct DbEntry {
    ChowKey key;
    std::array<int32_t, 6> weights;
    int32_t threshold;
    uint8_t k;
};

inline constexpr ChowKey pack_key(int k, int c0, const std::array<int, 6>& sorted_canon) {
    static_assert(64 < 128, "Chow parameter exceeds 7-bit capacity");
    ChowKey key = (static_cast<uint64_t>(k) << 49) | (static_cast<uint64_t>(c0) << 42);
    for (int i = 0; i < k; ++i) {
        key |= (static_cast<uint64_t>(sorted_canon[i]) << (35 - i * 7));
    }
    return key;
}

// База расширена до K=2, чтобы покрыть базовые функции (AND, OR, проекции)
//
// ИСПРАВЛЕНО (см. bdd/README.md §5.3, находка №2 — две записи были
// математически неверны, пересчитано вручную):
//  - "Проекция" (K=2, f(x0,x1)=x0, x1 фиктивна): sat=2 (assignments с x0=1),
//    ci[x0]=2 (все sat-присвоения имеют x0=1), ci[x1]=1 (из них ровно одно
//    имеет x1=1) -> вектор Чоу (2,1), не (2,0), как было. Плюс отдельная,
//    не отмеченная в README ошибка: с весами (1,0) правильный порог — 1
//    (sum=x0>=1 <=> x0=1), а не 2 (sum>=2 никогда не выполняется при x0 in
//    {0,1} — старая запись была константой 0, а не проекцией).
//  - "AND2 с фиктивной переменной" (K=3, f=x0 AND x1, x2 фиктивна): sat=2,
//    ci[x0]=ci[x1]=2, ci[x2]=1 -> вектор Чоу (2,2,1), не (2,2,0). Веса (1,1,0)
//    и порог 2 уже были верны — ошибка была только в ключе (sorted_canon),
//    из-за чего реальная AND2-с-фиктивной-переменной функция никогда не
//    находилась бы в базе (не false positive — verify_threshold_from_tt всё
//    равно защищал от него — а false negative: NeedFallback вместо Threshold).
inline constexpr std::array<DbEntry, 8> CHOW_DATABASE = {{
    // K=1
    { pack_key(1, 0, std::array<int, 6>{0, 0, 0, 0, 0, 0}), {{0, 0, 0, 0, 0, 0}}, 1, 1 }, // Константа 0
    { pack_key(1, 1, std::array<int, 6>{1, 0, 0, 0, 0, 0}), {{1, 0, 0, 0, 0, 0}}, 1, 1 }, // x0

    // K=2
    { pack_key(2, 1, std::array<int, 6>{1, 1, 0, 0, 0, 0}), {{1, 1, 0, 0, 0, 0}}, 2, 2 }, // AND2
    { pack_key(2, 2, std::array<int, 6>{2, 1, 0, 0, 0, 0}), {{1, 0, 0, 0, 0, 0}}, 1, 1 }, // Проекция (одна переменная фиктивна)
    { pack_key(2, 3, std::array<int, 6>{2, 2, 0, 0, 0, 0}), {{1, 1, 0, 0, 0, 0}}, 1, 1 }, // OR2

    // K=3
    { pack_key(3, 2, std::array<int, 6>{2, 2, 1, 0, 0, 0}), {{1, 1, 0, 0, 0, 0}}, 2, 3 }, // AND2 с фиктивной переменной
    { pack_key(3, 4, std::array<int, 6>{3, 3, 3, 0, 0, 0}), {{1, 1, 1, 0, 0, 0}}, 2, 3 }, // MAJ3
    { pack_key(3, 7, std::array<int, 6>{4, 4, 4, 0, 0, 0}), {{1, 1, 1, 0, 0, 0}}, 1, 3 }  // OR3
}};

static_assert([] {
    for (size_t i = 1; i < CHOW_DATABASE.size(); ++i) {
        if (CHOW_DATABASE[i-1].key > CHOW_DATABASE[i].key) return false;
    }
    return true;
}(), "CHOW_DATABASE must be sorted by key for std::lower_bound to work");

constexpr bool IS_FULL_DATABASE = false; // Переключить на true при подключении полной БД (2730 записей)

template <int K>
FastThresholdResult<K> identify_threshold_fast(
    const GlobalChow& chow,
    const std::array<bool, 6>& is_negative
) {
    std::array<int, 6> c_can{};
    for (int i = 0; i < K; ++i) {
        c_can[i] = is_negative[i]
            ? (static_cast<int>(chow.sat) - static_cast<int>(chow.ci[i]))
            : static_cast<int>(chow.ci[i]);
    }

    std::array<int, 6> var_order;
    std::iota(var_order.begin(), var_order.end(), 0);

    // Сортировка по УБЫВАНИЮ значений c_can, чтобы соответствовать формату CHOW_DATABASE
    auto cmp = [&](int a, int b) { return c_can[a] < c_can[b]; };
    auto cswap = [&](int i, int j) {
        if (cmp(var_order[i], var_order[j])) std::swap(var_order[i], var_order[j]);
    };

    // Оптимальная sorting network для 6 элементов (12 компараторов)
    cswap(0, 1); cswap(2, 3); cswap(4, 5);
    cswap(0, 2); cswap(1, 4); cswap(3, 5);
    cswap(1, 2); cswap(3, 4);
    cswap(0, 3); cswap(1, 5); cswap(2, 4);
    cswap(1, 2); cswap(3, 4);
    cswap(2, 3);

    std::array<int, 6> sorted_canon{};
    for (int i = 0; i < K; ++i) sorted_canon[i] = c_can[var_order[i]];

    const ChowKey key = pack_key(K, static_cast<int>(chow.sat), sorted_canon);

    auto it = std::lower_bound(CHOW_DATABASE.begin(), CHOW_DATABASE.end(), key,
        [](const DbEntry& entry, ChowKey k) { return entry.key < k; });

    if (it == CHOW_DATABASE.end() || it->key != key) {
        return FastThresholdResult<K>{{}, 0, IS_FULL_DATABASE ? ThresholdStatus::NotThreshold : ThresholdStatus::NeedFallback};
    }

    FastThresholdResult<K> result;
    result.status = ThresholdStatus::Threshold;
    result.threshold = it->threshold;

    for (int i = 0; i < K; ++i) {
        int orig_idx = var_order[i];
        int32_t w_can = it->weights[i];
        if (is_negative[orig_idx]) {
            result.weights[orig_idx] = -w_can;
            result.threshold -= w_can;
        } else {
            result.weights[orig_idx] = w_can;
        }
    }
    return result;
}

} // namespace detail

// ============================================================================
// Публичный API: bdd_to_thr
// ============================================================================

Result<Thr> bdd_to_thr(const Bdd& bdd) {
    ZoneScoped;

    try {
        uint32_t n_vars = bdd.n_vars();
        const sylvan::Bdd f_syl = bdd.raw();

        if (f_syl.isZero()) return ok(Thr(std::vector<int64_t>(n_vars, 0), 1));
        if (f_syl.isOne())  return ok(Thr(std::vector<int64_t>(n_vars, 0), 0));

        // ИСПРАВЛЕНО (см. bdd/README.md §5.3, находка №1): раньше гейтинг
        // шёл по n_vars (заявленной арности BDD), а не по K (реальному числу
        // РАЗЛИЧНЫХ переменных, встречающихся в приведённом BDD) — функция с
        // n_vars=7+, но фактически зависящая всего от K<=6 переменных
        // (фиктивные переменные физически не становятся узлами в reduced
        // BDD), никогда не попадала в рабочий путь. Обход для сбора
        // unique_vars — O(|BDD|), не O(2^n_vars), поэтому дёшево делать его
        // всегда, независимо от n_vars, и проверять K<=6 уже после.
        std::array<uint32_t, 6> unique_vars{};
        int unique_count = 0;
        bool too_many_vars = false;

        {
            std::vector<sylvan::Bdd> stack;
            std::vector<uint64_t> visited;

            stack.reserve(256);
            visited.reserve(256);

            stack.push_back(f_syl);

            while (!stack.empty()) {
                sylvan::Bdd curr = stack.back();
                stack.pop_back();

                uint64_t reg = curr.GetBDD() & ~detail::COMP_BIT;

                if (reg == 0 || reg == 1) continue;

                bool found = false;
                for (uint64_t v : visited) {
                    if (v == reg) { found = true; break; }
                }
                if (found) continue;

                visited.push_back(reg);

                uint32_t bdd_var = curr.TopVar();
                bool var_exists = false;
                for (int i = 0; i < unique_count; ++i) {
                    if (unique_vars[i] == bdd_var) { var_exists = true; break; }
                }

                if (!var_exists) {
                    if (unique_count < 6) {
                        unique_vars[unique_count++] = bdd_var;
                    } else {
                        too_many_vars = true;
                        break;
                    }
                }

                stack.push_back(curr.Then());
                stack.push_back(curr.Else());
            }
        }

        if (too_many_vars) {
            return fail<Thr>(ErrorCode::NotImplemented, "bdd_to_thr: ILP fallback для K > 6 находится в разработке");
        }

        // После обработки констант unique_count обязан быть > 0
        assert(unique_count > 0);

        int K = unique_count;
        std::sort(unique_vars.begin(), unique_vars.begin() + unique_count);

        // 1. Строим Truth Table ОДИН раз
        uint64_t truth = detail::build_truth_table(f_syl, K, unique_vars);

        // 2. Вычисляем Chow напрямую из TT
        detail::GlobalChow chow = detail::compute_chow_from_tt(truth, K);

        // 3. Проверяем унитарность по TT
        std::array<bool, 6> is_negative{};
        if (!detail::is_unate_from_tt(truth, K, is_negative)) {
            return fail<Thr>(ErrorCode::Unsupported, "bdd_to_thr: функция не является унитарной (и, следовательно, не пороговой)");
        }

        // 4. Lookup через шаблонную диспетчеризацию
        auto dispatch = [&](auto int_const) {
            constexpr int k = decltype(int_const)::value;
            return detail::identify_threshold_fast<k>(chow, is_negative);
        };

        detail::ThresholdStatus status = detail::ThresholdStatus::NeedFallback;
        std::vector<int64_t> final_weights;
        int32_t final_threshold = 0;

        switch (K) {
            case 1: {
                auto res = dispatch(std::integral_constant<int, 1>{});
                status = res.status;
                if (status == detail::ThresholdStatus::Threshold) {
                    final_weights = {res.weights[0]};
                    final_threshold = res.threshold;
                }
                break;
            }
            case 2: {
                auto res = dispatch(std::integral_constant<int, 2>{});
                status = res.status;
                if (status == detail::ThresholdStatus::Threshold) {
                    final_weights = {res.weights[0], res.weights[1]};
                    final_threshold = res.threshold;
                }
                break;
            }
            case 3: {
                auto res = dispatch(std::integral_constant<int, 3>{});
                status = res.status;
                if (status == detail::ThresholdStatus::Threshold) {
                    final_weights = {res.weights[0], res.weights[1], res.weights[2]};
                    final_threshold = res.threshold;
                }
                break;
            }
            case 4: {
                auto res = dispatch(std::integral_constant<int, 4>{});
                status = res.status;
                if (status == detail::ThresholdStatus::Threshold) {
                    final_weights = {res.weights[0], res.weights[1], res.weights[2], res.weights[3]};
                    final_threshold = res.threshold;
                }
                break;
            }
            case 5: {
                auto res = dispatch(std::integral_constant<int, 5>{});
                status = res.status;
                if (status == detail::ThresholdStatus::Threshold) {
                    final_weights.resize(5);
                    for (int i = 0; i < 5; ++i) final_weights[i] = res.weights[i];
                    final_threshold = res.threshold;
                }
                break;
            }
            case 6: {
                auto res = dispatch(std::integral_constant<int, 6>{});
                status = res.status;
                if (status == detail::ThresholdStatus::Threshold) {
                    final_weights.resize(6);
                    for (int i = 0; i < 6; ++i) final_weights[i] = res.weights[i];
                    final_threshold = res.threshold;
                }
                break;
            }
        }

        if (status == detail::ThresholdStatus::Threshold) {
            // 5. КРИТИЧЕСКИ ВАЖНО: Финальная верификация по TT
            if (!detail::verify_threshold_from_tt(truth, final_weights, final_threshold, K)) {
                return fail<Thr>(ErrorCode::Unsupported, "bdd_to_thr: функция не является пороговой (верификация не пройдена)");
            }

            // 6. Восстанавливаем глобальные веса размера n_vars
            std::vector<int64_t> global_weights(n_vars, 0);
            for (int i = 0; i < K; ++i) {
                global_weights[unique_vars[i]] = final_weights[i];
            }

            return ok(Thr(std::move(global_weights), final_threshold));
        } else if (status == detail::ThresholdStatus::NotThreshold) {
            return fail<Thr>(ErrorCode::Unsupported, "bdd_to_thr: функция не является пороговой");
        } else {
            return fail<Thr>(ErrorCode::NotImplemented, "bdd_to_thr: требуется полная база Чоу или ILP fallback");
        }

    } catch (const std::bad_alloc&) {
        return fail<Thr>(ErrorCode::OutOfMemory, "bdd_to_thr: исчерпана память");
    }
    // Общий catch(std::exception) намеренно удален, чтобы внутренние ошибки
    // (logic_error и т.д.) не маскировались под InvalidArgument, а проявлялись явно.
}

} // namespace bmm
