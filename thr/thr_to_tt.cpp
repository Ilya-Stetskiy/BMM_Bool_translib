#include "thr_to_tt.hpp"
#include <tracy/Tracy.hpp>
#include <omp.h>
#include <cstdint>
#include <algorithm>
#include <iterator>

namespace bmm {

Result<TruthTable> thr_to_tt(const Thr& thr) {
    ZoneScoped;

    const uint32_t n = thr.n_vars();

    // Защита от переполнения in-memory таблицы (согласно п.3 конвенций)
    if (n > kMaxTruthTableVars) {
        return fail<TruthTable>(ErrorCode::TooManyVariables, 
            "Количество переменных превышает 24");
    }

    TruthTable tt(n);

    // Если функция от 0 переменных, оцениваем константу
    if (n == 0) {
        if (thr.evaluate(Assignment{})) {
            // Используем .raw() для доступа к kitty::dynamic_truth_table
            *std::begin(tt.raw()) = 1ULL;
        }
        return ok(tt);
    }

    // Достаем сырые данные для горячего цикла
    const auto* weights = thr.weights().data();
    const auto threshold = thr.theta();

    // Получаем сырой указатель на внутренний массив 64-битных слов через raw()
    auto* tt_data = &(*std::begin(tt.raw()));
    
    // Количество 64-битных блоков (минимум 1)
    const uint64_t num_blocks = (n < 6) ? 1ULL : (1ULL << (n - 6));

    // ИСПРАВЛЕНО (см. thr/README.md §2, находка): раньше omp parallel for
    // включался безусловно — на малых n (единицы-десятки блоков) накладные
    // расходы OpenMP-региона доминировали над самой работой (замер:
    // speedup=0.0013 при n=8, т.е. "параллельная" версия в 754 раза
    // медленнее последовательной). Порог согласован с тем же 2^18 по общему
    // числу точек (2^n), который уже используется в anf/anf_to_tt.cpp и
    // anf/tt_to_anf.cpp для аналогичного паттерна "OpenMP над плоским
    // массивом из 2^n элементов" — num_blocks*64 = 2^n, отсюда порог в
    // блоках: 2^18/64 = 2^12.
    constexpr uint64_t kParallelThresholdBlocks = 1ULL << 12;

    // OpenMP-распараллеливание (Правило №6 конвенций)
    #pragma omp parallel for if(num_blocks > kParallelThresholdBlocks)
    for (int64_t block_idx = 0; block_idx < static_cast<int64_t>(num_blocks); ++block_idx) {
        uint64_t current_block = 0;
        const uint64_t start_minterm = static_cast<uint64_t>(block_idx) << 6;
        
        // Для n < 6 обрабатываем только 2^n бит, иначе все 64 бита в блоке
        const uint64_t limit = (n < 6) ? (1ULL << n) : 64ULL;

        for (uint64_t bit_idx = 0; bit_idx < limit; ++bit_idx) {
            const uint64_t minterm = start_minterm + bit_idx;
            int64_t sum = 0;

            // LSB_FIRST: переменная v - это v-й бит числа minterm
            for (uint32_t v = 0; v < n; ++v) {
                if ((minterm >> v) & 1ULL) {
                    sum += weights[v];
                }
            }

            if (sum >= threshold) {
                current_block |= (1ULL << bit_idx);
            }
        }
        
        // Запись готового блока напрямую в память (потокобезопасно, т.к. индексы не пересекаются)
        tt_data[block_idx] = current_block;
    }

    return ok(tt);
}

} // namespace bmm