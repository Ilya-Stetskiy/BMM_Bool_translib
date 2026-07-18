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

Result<TruthTable> bdd_to_tt(const Bdd& f) {
    ZoneScoped;

    try {
        const uint32_t n = f.n_vars();
        if (n > kMaxTruthTableVars) {
            return fail<TruthTable>(ErrorCode::TooManyVariables,
                "bdd_to_tt: n > " + std::to_string(kMaxTruthTableVars));
        }

        sylvan::Bdd f_syl = f.raw();
        TruthTable tt(n);
        
        if (f_syl.isZero()) return ok(std::move(tt));

        // Прямой доступ к памяти kitty::dynamic_truth_table
        uint64_t* data = tt.raw()._bits.data();

        // Фрейм хранит узел BDD, битовую маску зафиксированных переменных
        // и их конкретное значение для текущего пути.
        struct Frame {
            sylvan::Bdd node;
            uint64_t mask;
            uint64_t value;
        };

        std::vector<Frame> stack;
        stack.reserve(64); 
        stack.push_back(Frame{ f_syl, 0, 0 });

        const uint64_t universe = (n >= 64) ? ~uint64_t{0} : ((uint64_t{1} << n) - 1);

        while (!stack.empty()) {
            Frame curr = std::move(stack.back());
            stack.pop_back();

            if (curr.node.isZero()) continue;

            if (curr.node.isOne()) {
                // Если мы дошли до One, все переменные, которые не попали в mask, 
                // могут быть любыми (don't cares). Итерируемся по всем их комбинациям.
                const uint64_t floating = (~curr.mask) & universe;
                uint64_t sub = floating;
                
                do {
                    uint64_t idx = curr.value | sub;
                    data[idx >> 6] |= (uint64_t{1} << (idx & 63));
                    sub = (sub - 1) & floating;
                } while (sub != floating);
                
                continue;
            }

            const uint32_t v = curr.node.TopVar();
            BMM_ASSERT(v < n && "BDD variable index out of bounds");
            
            // Ветвь Else (переменная v = 0)
            stack.push_back(Frame{ curr.node.Else(), curr.mask | (uint64_t{1} << v), curr.value });
            
            // Ветвь Then (переменная v = 1)
            stack.push_back(Frame{ curr.node.Then(), curr.mask | (uint64_t{1} << v), curr.value | (uint64_t{1} << v) });
        }

        return ok(std::move(tt));

    } catch (const std::exception& e) {
        return fail<TruthTable>(ErrorCode::InvalidArgument,
            std::string("bdd_to_tt internal error: ") + e.what());
    }
}

} // namespace bmm