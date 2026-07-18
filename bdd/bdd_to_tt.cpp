#include "bdd_to_tt.hpp"

#include <tracy/Tracy.hpp>
#include <sylvan_obj.hpp>

#include <vector>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

#ifndef BMM_ASSERT
#include <cassert>
#define BMM_ASSERT(x) assert(x)
#endif

namespace bmm {

namespace {

uint64_t mask_bits(unsigned n)
{
    if (n == 64)
        return ~uint64_t{0};

    return (uint64_t{1} << n) - 1;
}

/**
 * Высокооптимизированное блочное заполнение таблицы истинности.
 * Группирует свободные (don't care) биты в непрерывные диапазоны памяти
 * для максимального кэш-эффекта и автовекторизации компилятором.
 */
void fill_masked(
    uint64_t* bits,
    uint64_t fixed_mask,
    uint64_t fixed_vals,
    uint32_t n)
{
    if (n < 6) {
        // Все переменные умещаются внутри одного 64-битного машинного слова
        uint64_t word_mask = 0;
        uint64_t free_mask = mask_bits(n) & ~fixed_mask;
        uint64_t sub = 0;
        do {
            word_mask |= (1ULL << (fixed_vals | sub));
            sub = (sub - free_mask) & free_mask;
        } while (sub != 0);

        bits[0] |= word_mask;
    } else {
        // Разделяем биты на внутрисловные (0-5) и межсловные (>=6)
        uint64_t low_mask = fixed_mask & 0x3F;
        uint64_t low_vals = fixed_vals & 0x3F;
        uint64_t high_mask = fixed_mask >> 6;
        uint64_t high_vals = fixed_vals >> 6;
        uint64_t high_limit = uint64_t{1} << (n - 6);

        // 1. Формируем 64-битную маску паттерна внутри одного слова
        uint64_t word_mask = 0;
        uint64_t free_low = 0x3F & ~low_mask;
        uint64_t sub_low = 0;
        do {
            word_mask |= (1ULL << (low_vals | sub_low));
            sub_low = (sub_low - free_low) & free_low;
        } while (sub_low != 0);

        // 2. Определяем свободные биты среди индексов слов
        uint64_t free_high = (high_limit - 1) & ~high_mask;

        // Находим размер МАКСИМАЛЬНОГО непрерывного блока слов (bitwise-трюк)
        uint64_t block_size = (~free_high) & (free_high + 1);
        if (block_size > high_limit) {
            block_size = high_limit;
        }

        // Выделяем оставшиеся (разреженные) свободные биты старших уровней
        uint64_t rest_free_high = free_high & ~(block_size - 1);
        uint64_t sub_high = 0;

        // Плотный цикл по непрерывным блокам — идеален для кэша процессора
        do {
            uint64_t start_w = high_vals | sub_high;
            
            // Этот участок компилятор разворачивает в SIMD/векторные инструкции записи
            for (uint64_t w = 0; w < block_size; ++w) {
                bits[start_w + w] |= word_mask;
            }

            sub_high = (sub_high - rest_free_high) & rest_free_high;
        } while (sub_high != 0);
    }
}

}

Result<TruthTable> bdd_to_tt(const Bdd& f)
{
    ZoneScoped;

    try {
        uint32_t n = f.n_vars();

        if (n > kMaxTruthTableVars)
        {
            return fail<TruthTable>(
                ErrorCode::TooManyVariables,
                "bdd_to_tt: too many vars");
        }

        TruthTable tt(n);

        if (f.raw().isZero())
            return ok(std::move(tt));

        uint64_t* data = tt.raw()._bits.data();

        // Кадр стека хранит только состояние для РЕАЛЬНЫХ узлов BDD
        struct Frame {
            sylvan::Bdd node;
            uint64_t fixed_mask; // Маска жестко зафиксированных переменных
            uint64_t fixed_vals; // Конкретные значения этих переменных
        };

        std::vector<Frame> stack;
        stack.reserve(n * 2); // Заранее аллоцируем память, чтобы избежать реаллокаций стека
        stack.push_back({ f.raw(), 0, 0 });

        while (!stack.empty())
        {
            Frame cur = std::move(stack.back());
            stack.pop_back();

            if (cur.node.isZero())
                continue;

            // Дошли до терминала 1 -> делаем быструю многомерную диапазонную заливку
            if (cur.node.isOne())
            {
                fill_masked(data, cur.fixed_mask, cur.fixed_vals, n);
                continue;
            }

            uint32_t var = cur.node.TopVar();
            BMM_ASSERT(var < n);

            uint64_t bit = 1ULL << var;

            // Ветка Else (переменная на уровне var равна 0)
            stack.push_back({
                cur.node.Else(),
                cur.fixed_mask | bit,
                cur.fixed_vals // бит var остается нулевым
            });

            // Ветка Then (переменная на уровне var равна 1)
            stack.push_back({
                cur.node.Then(),
                cur.fixed_mask | bit,
                cur.fixed_vals | bit // выставляем бит var в 1
            });
        }

        return ok(std::move(tt));

    }
    catch(const std::exception& e)
    {
        return fail<TruthTable>(
            ErrorCode::InvalidArgument,
            e.what());
    }
}

}