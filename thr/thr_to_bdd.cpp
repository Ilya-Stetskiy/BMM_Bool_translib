#include "thr_to_bdd.hpp"
#include <tracy/Tracy.hpp>

#include <vector>
#include <unordered_map>
#include <algorithm>

namespace bmm {

// Вспомогательный функтор для хеширования состояния DP
struct ThrStateHash {
    std::size_t operator()(const std::pair<uint32_t, int64_t>& state) const {
        return std::hash<uint32_t>()(state.first) ^ (std::hash<int64_t>()(state.second) << 1);
    }
};

Result<Bdd> thr_to_bdd(const Thr& thr) {
    ZoneScoped;
    
    try {
        const uint32_t n = thr.n_vars();
        
        if (n == 0) {
            // Исправлено: передаём 'n' вторым аргументом
            return ok(Bdd(thr.theta() <= 0 ? sylvan::Bdd::bddOne() : sylvan::Bdd::bddZero(), n));
        }

        // Предрасчёт для отсечения граней (bounding)
        std::vector<int64_t> max_remain(n + 1, 0);
        std::vector<int64_t> min_remain(n + 1, 0);
        
        for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
            int64_t w = thr.weights()[i];
            max_remain[i] = max_remain[i + 1] + std::max<int64_t>(0, w);
            min_remain[i] = min_remain[i + 1] + std::min<int64_t>(0, w);
        }

        // Кеш для мемоизации
        std::unordered_map<std::pair<uint32_t, int64_t>, sylvan::Bdd, ThrStateHash> memo;

        // Рекурсивное построение графа BDD
        auto build_dp = [&](auto& self, uint32_t i, int64_t current_t) -> sylvan::Bdd {
            if (current_t <= min_remain[i]) return sylvan::Bdd::bddOne();
            if (current_t > max_remain[i]) return sylvan::Bdd::bddZero();
            
            if (i == n) {
                return current_t <= 0 ? sylvan::Bdd::bddOne() : sylvan::Bdd::bddZero();
            }

            auto state = std::make_pair(i, current_t);
            if (auto it = memo.find(state); it != memo.end()) {
                return it->second;
            }

            int64_t w = thr.weights()[i];
            
            sylvan::Bdd high = self(self, i + 1, current_t - w);
            sylvan::Bdd low  = self(self, i + 1, current_t);

            sylvan::Bdd res = sylvan::Bdd::bddVar(i).Ite(high, low);
            
            memo[state] = res;
            return res;
        };

        // Исправлено: передаём 'n' вторым аргументом вместе с построенным корнем
        return ok(Bdd(build_dp(build_dp, 0, thr.theta()), n));

    } catch (const std::bad_alloc&) {
        return fail<Bdd>(ErrorCode::OutOfMemory, "thr_to_bdd: нехватка памяти при сборке графа");
    }
}

} // namespace bmm