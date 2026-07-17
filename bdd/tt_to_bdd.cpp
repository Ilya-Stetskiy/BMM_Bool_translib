#include "tt_to_bdd.hpp"

#include <tracy/Tracy.hpp>
#include <sylvan_obj.hpp>
#include <kitty/operations.hpp>
#include <parallel_hashmap/phmap.h>
#include <cstdint>

namespace bmm {

namespace {

// Используем phmap из mockturtle — он в 3-5 раз быстрее std::unordered_map
using BddCache = phmap::flat_hash_map<uint64_t, sylvan::Bdd>;

sylvan::Bdd build_bdd_recursive(
    const kitty::dynamic_truth_table& tt,
    uint32_t var,
    uint64_t block_start,
    BddCache& cache
) {
    // Базовый случай: var == 0
    if (var == 0) {
        bool val0 = kitty::get_bit(tt, block_start);
        bool val1 = kitty::get_bit(tt, block_start + 1);
        
        if (val0 == val1) {
            return val0 ? sylvan::Bdd::bddOne() : sylvan::Bdd::bddZero();
        } else {
            sylvan::Bdd x0 = sylvan::Bdd::bddVar(0);
            return val0 ? !x0 : x0;
        }
    }

    // Быстрый ключ: var в старших 8 битах, block_start в младших 56 битах
    uint64_t key = ((uint64_t)var << 56) | block_start;
    
    // Проверка кэша
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    uint64_t half = 1ULL << var;
    
    sylvan::Bdd low  = build_bdd_recursive(tt, var - 1, block_start, cache);
    sylvan::Bdd high = build_bdd_recursive(tt, var - 1, block_start + half, cache);

    // Pruning: если кофакторы идентичны
    if (low == high) {
        cache[key] = low;
        return low;
    }

    // Разложение Шеннона
    sylvan::Bdd x = sylvan::Bdd::bddVar(var);
    sylvan::Bdd res = x.Ite(high, low);

    cache[key] = res;
    return res;
}

} // namespace

Result<Bdd> tt_to_bdd(const TruthTable& tt) {
    ZoneScoped;

    uint32_t n = tt.n_vars();

    if (n > kMaxTruthTableVars) {
        return fail<Bdd>(ErrorCode::TooManyVariables, 
            "tt_to_bdd: n > " + std::to_string(kMaxTruthTableVars));
    }

    if (n == 0) {
        bool val = kitty::get_bit(tt.raw(), 0);
        return ok(Bdd(val ? sylvan::Bdd::bddOne() : sylvan::Bdd::bddZero(), 0));
    }

    // phmap с предвыделением памяти
    BddCache cache;
    cache.reserve(1 << std::min(n, 20u)); // Резервируем под 2^min(n,20) записей

    const kitty::dynamic_truth_table& raw_tt = tt.raw();
    sylvan::Bdd root = build_bdd_recursive(raw_tt, n - 1, 0, cache);

    return ok(Bdd(std::move(root), n));
}

} // namespace bmm