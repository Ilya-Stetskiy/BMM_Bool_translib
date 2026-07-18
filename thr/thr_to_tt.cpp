#include "thr_to_tt.hpp"
#include <tracy/Tracy.hpp>
#include <omp.h>
#include <cstdint>
#include <algorithm>
#include <iterator>
#include <bit> // Для std::countr_zero (C++20)

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

    // OpenMP: порог n >= 18. При n < 18 накладные расходы на треды превышают 
    // пользу от параллелизма (например, для n=8-16 параллельный код медленнее)
    #pragma omp parallel for if(n >= 18) schedule(static)
    for (int64_t block_idx = 0; block_idx < static_cast<int64_t>(num_blocks); ++block_idx) {
        uint64_t current_block = 0;
        const uint64_t start_minterm = static_cast<uint64_t>(block_idx) << 6;
        
        // 1. Вычисляем стартовую сумму для блока (по старшим битам >= 6)
        int64_t current_sum = 0;
        for (uint32_t v = 6; v < n; ++v) {
            if ((start_minterm >> v) & 1ULL) {
                current_sum += weights[v];
            }
        }
        
        const uint32_t limit = (n < 6) ? (1U << n) : 64U;

        // 2. Инкрементальный проход кодом Грея внутри 64-битного блока
        for (uint32_t i = 0; i < limit; ++i) {
            // g - локальный индекс минтерма (от 0 до 63)
            const uint32_t g = i ^ (i >> 1); 
            
            if (i > 0) {
                // std::countr_zero дает индекс бита, который изменился на этом шаге
                const uint32_t changed_bit = std::countr_zero(i);
                
                // Проверяем, установился бит в 1 или сбросился в 0
                if ((g >> changed_bit) & 1U) {
                    current_sum += weights[changed_bit];
                } else {
                    current_sum -= weights[changed_bit];
                }
            }

            // Записываем результат: ставим 1 на место g, если порог достигнут
            if (current_sum >= threshold) {
                current_block |= (1ULL << g);
            }
        }
        
        // Запись готового блока напрямую в память
        tt_data[block_idx] = current_block;
    }

    return ok(tt);
}

} // namespace bmm