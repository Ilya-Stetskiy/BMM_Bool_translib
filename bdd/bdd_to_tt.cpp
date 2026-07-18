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

struct Frame {
    sylvan::Bdd node;
    uint64_t fixed_mask; // Маска жёстко зафиксированных переменных
    uint64_t fixed_vals; // Конкретные значения этих переменных
};

// Переиспользуемый стек потока: исключает аллокации в куче на каждом вызове bdd_to_tt
thread_local std::vector<Frame> tls_stack;

/**
 * Высокоэффективное заполнение куба свободных переменных.
 * Разделяет логику на внутрисловную генерацию маски и межсловную заливку.
 */
void fill_cube(
    uint64_t* bits,
    uint64_t fixed_mask,
    uint64_t fixed_vals,
    uint32_t n)
{
    // 1. Формируем 64-битную маску паттерна внутри одного слова
    uint64_t low_limit_mask = (n < 6) ? ((1ULL << n) - 1) : 0x3F;
    uint64_t low_fixed_mask = fixed_mask & 0x3F;
    uint64_t low_fixed_vals = fixed_vals & 0x3F;
    uint64_t low_free_mask = low_limit_mask & ~low_fixed_mask;

    uint64_t word_mask = 0;
    uint64_t sub_low = low_free_mask;
    do {
        word_mask |= (1ULL << (low_fixed_vals | sub_low));
        sub_low = (sub_low - 1) & low_free_mask;
    } while (sub_low != low_free_mask);

    // Если таблица истинности занимает меньше одного 64-битного слова
    if (n < 6) {
        bits[0] |= word_mask;
        return;
    }

    // 2. Распределяем сгенерированную маску по словам таблицы истинности
    uint64_t total_words = 1ULL << (n - 6);
    uint64_t high_var_mask = total_words - 1;
    uint64_t high_fixed_mask = (fixed_mask >> 6) & high_var_mask;
    uint64_t high_fixed_vals = (fixed_vals >> 6) & high_var_mask;
    uint64_t high_free_mask = high_var_mask & ~high_fixed_mask;

    // ВАЖНЫЙ ХОТПАД: Все старшие переменные свободны (частый кейс для нижних уровней BDD).
    // Этот цикл идеально векторизуется компилятором через SIMD инструкции.
    if (high_fixed_mask == 0) {
        for (uint64_t w = 0; w < total_words; ++w) {
            bits[w] |= word_mask;
        }
        return;
    }

    // Разреженный кейс: обходим только те слова, которые подходят под свободные старшие биты
    uint64_t sub_high = high_free_mask;
    do {
        uint64_t w = high_fixed_vals | sub_high;
        bits[w] |= word_mask;
        sub_high = (sub_high - 1) & high_free_mask;
    } while (sub_high != high_free_mask);
}

} // namespace

Result<TruthTable> bdd_to_tt(const Bdd& f)
{
    ZoneScoped;

    try {
        uint32_t n = f.n_vars();

        if (n > kMaxTruthTableVars) {
            return fail<TruthTable>(
                ErrorCode::TooManyVariables,
                "bdd_to_tt: too many vars");
        }

        TruthTable tt(n);
        sylvan::Bdd root = f.raw();

        if (root.isZero())
            return ok(std::move(tt));

        uint64_t* data = tt.raw()._bits.data();

        // Сбрасываем размер глобального стека потока, сохраняя выделенную ранее ёмкость
        tls_stack.clear();
        tls_stack.push_back({ root, 0, 0 });

        while (!tls_stack.empty())
        {
            Frame cur = std::move(tls_stack.back());
            tls_stack.pop_back();

            if (cur.node.isZero())
                continue;

            // Дошли до терминала 1 -> разворачиваем куб свободных переменных в память
            if (cur.node.isOne()) {
                fill_cube(data, cur.fixed_mask, cur.fixed_vals, n);
                continue;
            }

            // level != переменная, если f построен с нестандартным порядком
            // уровней (см. Bdd::var_at_level в core/common.hpp — сейчас
            // единственный такой производитель Bdd — anf_to_bdd). Эта версия
            // портирована с ветки origin/egor, где var_to_level ещё не
            // существовал — без этого маппинга функция тихо возвращала бы
            // неверную TT для любого Bdd от anf_to_bdd() с нетривиальной
            // физической перестановкой уровня (умолчание — VariableOrderHeuristic::Force).
            uint32_t var = f.var_at_level(cur.node.TopVar());
            BMM_ASSERT(var < n);

            uint64_t bit = 1ULL << var;

            // Ветка Else (переменная var равна 0)
            tls_stack.push_back({
                cur.node.Else(),
                cur.fixed_mask | bit,
                cur.fixed_vals // бит var остается нулевым
            });

            // Ветка Then (переменная var равна 1)
            tls_stack.push_back({
                cur.node.Then(),
                cur.fixed_mask | bit,
                cur.fixed_vals | bit // выставляем бит var в 1
            });
        }

        return ok(std::move(tt));

    }
    catch (const std::bad_alloc&) {
        return fail<TruthTable>(
            ErrorCode::OutOfMemory,
            "bdd_to_tt: исчерпана память");
    }
    catch(const std::exception& e) {
        return fail<TruthTable>(
            ErrorCode::InvalidArgument,
            e.what());
    }
}

} // namespace bmm
