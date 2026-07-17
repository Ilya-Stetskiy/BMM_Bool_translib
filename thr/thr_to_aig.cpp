#include "thr/thr_to_aig.hpp"

#include <mockturtle/networks/aig.hpp>
#include <tbb/task_group.h>
#include <tbb/concurrent_hash_map.h>

#include <vector>
#include <mutex>
#include <algorithm>
#include <cstdint>
#include <array>
#include <functional>

namespace bmm {

using Signal = mockturtle::aig_network::signal;

// Прячем вспомогательные структуры в анонимный namespace, 
// чтобы они были видны только внутри этого .cpp файла
namespace {
    struct FA_Input {
        uint32_t a, b, c;
    };

    struct FA_Hash {
        std::size_t hash(const FA_Input& i) const {
            return std::hash<uint32_t>()(i.a) ^ 
                  (std::hash<uint32_t>()(i.b) << 1) ^ 
                  (std::hash<uint32_t>()(i.c) << 2);
        }
        bool equal(const FA_Input& i1, const FA_Input& i2) const {
            return i1.a == i2.a && i1.b == i2.b && i1.c == i2.c;
        }
    };
} // namespace

Result<Aig> thr_to_aig(const Thr& thr) {
    try {
        const uint32_t n = thr.n_vars();
        mockturtle::aig_network ntk;
        
        if (n == 0) {
            auto sig = ntk.get_constant(thr.theta() <= 0);
            ntk.create_po(sig);
            return ok<Aig>(Aig{ntk});
        }

        std::vector<Signal> pis(n);
        for (uint32_t i = 0; i < n; ++i) {
            pis[i] = ntk.create_pi();
        }

        // 1. Приведение к неотрицательным весам
        int64_t theta_adj = thr.theta();
        std::vector<uint64_t> pos_weights(n);
        std::vector<Signal> var_signals(n);
        
        uint64_t max_sum = 0;

        for (uint32_t i = 0; i < n; ++i) {
            int64_t current_weight = thr.weights()[i];
            if (current_weight < 0) {
                // Замена x_i -> (1 - x_i)
                pos_weights[i] = static_cast<uint64_t>(-current_weight);
                theta_adj -= current_weight;
                var_signals[i] = ntk.create_not(pis[i]);
            } else {
                pos_weights[i] = static_cast<uint64_t>(current_weight);
                var_signals[i] = pis[i];
            }
            max_sum += pos_weights[i];
        }

        // Раннее отсечение
        if (theta_adj <= 0) {
            auto sig = ntk.get_constant(true);
            ntk.create_po(sig);
            return ok<Aig>(Aig{ntk});
        }
        if (theta_adj > static_cast<int64_t>(max_sum)) {
            auto sig = ntk.get_constant(false);
            ntk.create_po(sig);
            return ok<Aig>(Aig{ntk});
        }

        // Разрядность для хранения сумм (B)
        int B = max_sum > 0 ? (64 - __builtin_clzll(max_sum)) : 1;

        std::mutex ntk_mutex;
        
        auto safe_not = [&](Signal a) {
            return ntk.create_not(a);
        };
        
        auto safe_and = [&](Signal a, Signal b) {
            std::lock_guard<std::mutex> lock(ntk_mutex);
            return ntk.create_and(a, b);
        };
        
        auto safe_or = [&](Signal a, Signal b) {
            return safe_not(safe_and(safe_not(a), safe_not(b)));
        };
        
        auto safe_xor = [&](Signal a, Signal b) {
            Signal term1 = safe_and(a, safe_not(b));
            Signal term2 = safe_and(safe_not(a), b);
            return safe_or(term1, term2);
        };

        using FAMemoMap = tbb::concurrent_hash_map<FA_Input, std::pair<Signal, Signal>, FA_Hash>;
        FAMemoMap fa_memo;

        // Полный сумматор с мемоизацией
        auto full_adder = [&](Signal a, Signal b, Signal cin) -> std::pair<Signal, Signal> {
            std::array<uint32_t, 3> idxs = {ntk.node_to_index(ntk.get_node(a)), 
                                            ntk.node_to_index(ntk.get_node(b)), 
                                            ntk.node_to_index(ntk.get_node(cin))};
            std::sort(idxs.begin(), idxs.end());
            FA_Input key{idxs[0], idxs[1], idxs[2]};

            FAMemoMap::accessor acc;
            if (fa_memo.insert(acc, key)) {
                Signal s1 = safe_xor(a, b);
                Signal sum = safe_xor(s1, cin);
                
                Signal c1 = safe_and(a, b);
                Signal c2 = safe_and(cin, s1);
                Signal cout = safe_or(c1, c2);
                
                acc->second = {sum, cout};
            }
            return acc->second;
        };

        // Сложение двух векторов (шириной B)
        auto add_bitvectors = [&](const std::vector<Signal>& vecA, const std::vector<Signal>& vecB) -> std::vector<Signal> {
            std::vector<Signal> sum(vecB.size());
            Signal cin = ntk.get_constant(false);
            
            for (size_t i = 0; i < vecB.size(); ++i) {
                auto [s, cout] = full_adder(vecA[i], vecB[i], cin);
                sum[i] = s;
                cin = cout;
            }
            return sum;
        };

        // 2. Инициализация листьев дерева сумматоров
        std::vector<std::vector<Signal>> bitvecs(n, std::vector<Signal>(B, ntk.get_constant(false)));
        for (uint32_t i = 0; i < n; ++i) {
            for (int bit = 0; bit < B; ++bit) {
                if ((pos_weights[i] >> bit) & 1) {
                    bitvecs[i][bit] = var_signals[i];
                }
            }
        }

        // 3. Рекурсивное построение дерева сумматоров через TBB
        std::function<std::vector<Signal>(int, int)> build_tree = [&](int l, int r) -> std::vector<Signal> {
            if (l == r) return bitvecs[l];
            
            int mid = l + (r - l) / 2;
            std::vector<Signal> left_sum, right_sum;
            
            tbb::task_group tg;
            tg.run([&]() { left_sum = build_tree(l, mid); });
            tg.run([&]() { right_sum = build_tree(mid + 1, r); });
            tg.wait();
            
            return add_bitvectors(left_sum, right_sum);
        };

        std::vector<Signal> final_sum = build_tree(0, n - 1);

        // 4. Компаратор: проверяем, что final_sum >= theta_adj
        uint64_t T = static_cast<uint64_t>(theta_adj);
        Signal ge = ntk.get_constant(true);
        
        for (int i = 0; i < B; ++i) {
            bool t_bit = (T >> i) & 1;
            if (t_bit) {
                ge = safe_and(final_sum[i], ge);
            } else {
                ge = safe_or(final_sum[i], ge);
            }
        }

        ntk.create_po(ge);

        return ok<Aig>(Aig{ntk});
    } 
    catch (const std::bad_alloc&) {
        return fail<Aig>(ErrorCode::OutOfMemory, "Memory exhausted during adder-tree AIG construction");
    }
}

} // namespace bmm