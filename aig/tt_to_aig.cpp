#include "tt_to_aig.hpp"

#include <tracy/Tracy.hpp>
#include <tbb/concurrent_hash_map.h>
#include <tbb/task_group.h>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <functional>
#include <iostream>

namespace bmm {

namespace {

struct MemoKey {
    kitty::dynamic_truth_table tt;
    uint32_t var_idx;
};

struct MemoKeyCompare {
    static bool equal(const MemoKey& a, const MemoKey& b) {
        return a.var_idx == b.var_idx && a.tt == b.tt;
    }
    static size_t hash(const MemoKey& key) {
        size_t h = std::hash<uint32_t>{}(key.var_idx);
        for (auto it = key.tt.begin(); it != key.tt.end(); ++it) {
            h ^= std::hash<uint64_t>{}(*it) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

struct NodeEntry {
    std::atomic<bool> ready{false};
    std::mutex mtx;
    mockturtle::aig_network::signal val;
};

using MemoMap = tbb::concurrent_hash_map<MemoKey, std::shared_ptr<NodeEntry>, MemoKeyCompare>;

} // namespace

Result<Aig> tt_to_aig(const TruthTable& tt) {
    ZoneScoped;
    const uint32_t n = tt.n_vars();
    if (n > kMaxTruthTableVars) {
        return fail<Aig>(ErrorCode::TooManyVariables, "tt_to_aig: слишком много переменных");
    }

    mockturtle::aig_network net;

    // Create PI signals
    std::vector<mockturtle::aig_network::signal> pi_signals(n);
    for (uint32_t i = 0; i < n; ++i) {
        pi_signals[i] = net.create_pi();
    }

    MemoMap memo;
    std::mutex net_mutex;

    // Thread-safe wrapper functions for modifying the shared mockturtle AIG network
    auto get_constant_safe = [&](bool value) {
        std::lock_guard<std::mutex> lock(net_mutex);
        return net.get_constant(value);
    };

    auto create_and_safe = [&](mockturtle::aig_network::signal a, mockturtle::aig_network::signal b) {
        std::lock_guard<std::mutex> lock(net_mutex);
        return net.create_and(a, b);
    };

    auto create_or_safe = [&](mockturtle::aig_network::signal a, mockturtle::aig_network::signal b) {
        std::lock_guard<std::mutex> lock(net_mutex);
        return net.create_or(a, b);
    };

    std::function<mockturtle::aig_network::signal(const kitty::dynamic_truth_table&, uint32_t)> build_aig_rec =
        [&](const kitty::dynamic_truth_table& current_tt, uint32_t var_idx) -> mockturtle::aig_network::signal {
        
        // 1. Const checks
        if (kitty::is_const0(current_tt)) {
            return get_constant_safe(false);
        }
        if (kitty::is_const0(~current_tt)) {
            return get_constant_safe(true);
        }

        // 2. Base case
        if (var_idx == 0) {
            if (kitty::get_bit(current_tt, 1)) {
                return pi_signals[0];
            } else {
                return !pi_signals[0];
            }
        }

        // 3. Memoization using (tt, var_idx) key to prevent deadlock when a cofactor is identical to its parent
        MemoKey key{current_tt, var_idx};
        MemoMap::accessor acc;
        bool is_new = memo.insert(acc, key);
        if (is_new) {
            acc->second = std::make_shared<NodeEntry>();
        }
        auto entry = acc->second;
        acc.release();

        if (entry->ready) {
            return entry->val;
        }

        std::lock_guard<std::mutex> lock(entry->mtx);
        if (entry->ready) {
            return entry->val;
        }

        // 4. Shannon decomposition cofactors
        uint32_t split_var = var_idx - 1;
        auto tt_lo = kitty::cofactor0(current_tt, split_var);
        auto tt_hi = kitty::cofactor1(current_tt, split_var);

        mockturtle::aig_network::signal lo_signal, hi_signal;

        // Parallelize only at upper levels to reduce TBB task creation overhead and prevent scheduler exhaustion
        if (var_idx >= 12) {
            tbb::task_group tg;
            tg.run([&] { lo_signal = build_aig_rec(tt_lo, split_var); });
            hi_signal = build_aig_rec(tt_hi, split_var);
            tg.wait();
        } else {
            lo_signal = build_aig_rec(tt_lo, split_var);
            hi_signal = build_aig_rec(tt_hi, split_var);
        }

        auto s = pi_signals[split_var];
        auto term_hi = create_and_safe(s, hi_signal);
        auto term_lo = create_and_safe(!s, lo_signal);
        auto result_signal = create_or_safe(term_hi, term_lo);

        entry->val = result_signal;
        entry->ready = true;
        return result_signal;
    };

    auto final_signal = build_aig_rec(tt.raw(), n);
    net.create_po(final_signal);

    return ok<Aig>(Aig(std::move(net)));
}

} // namespace bmm
