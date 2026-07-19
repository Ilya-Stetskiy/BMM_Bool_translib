#pragma once

// Общая логика работы с вектором Чоу (Chow parameters) для K<=6 переменных,
// используемая и рабочим кодом (bdd_to_thr.cpp), и офлайн-генератором
// справочной таблицы (verify/diag_chow_database_gen.cpp). Вынесено в
// отдельный заголовок, чтобы генератор и runtime-путь использовали БУКВАЛЬНО
// одну и ту же реализацию pack_key/compute_chow/is_unate/verify — иначе
// расхождение в деталях канонизации между "как строили таблицу" и "как её
// читают" было бы совершенно незаметным до первого ложного результата.

#include <array>
#include <cstdint>
#include <bit>
#include <vector>

namespace bmm {
namespace chow_detail {

struct GlobalChow {
    uint32_t sat;
    std::array<uint32_t, 6> ci;
};

inline GlobalChow compute_chow_from_tt(uint64_t truth, int K) {
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

inline bool is_unate_from_tt(uint64_t truth, int K, std::array<bool, 6>& is_negative) {
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

inline bool verify_threshold_from_tt(
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

using ChowKey = uint64_t;

struct DbEntry {
    ChowKey key;
    std::array<int32_t, 6> weights;
    int32_t threshold;
    uint8_t k;
};

// 7 бит на поле (k, sat, ci[0..5]) — для K<=6 sat<=64 умещается в 7 бит
// (2^7=128), значения ci тоже (ci<=sat<=64).
inline constexpr ChowKey pack_key(int k, int c0, const std::array<int, 6>& sorted_canon) {
    ChowKey key = (static_cast<uint64_t>(k) << 49) | (static_cast<uint64_t>(c0) << 42);
    for (int i = 0; i < k; ++i) {
        key |= (static_cast<uint64_t>(sorted_canon[i]) << (35 - i * 7));
    }
    return key;
}

}  // namespace chow_detail
}  // namespace bmm
