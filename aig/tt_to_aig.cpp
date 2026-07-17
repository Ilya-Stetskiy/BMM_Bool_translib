#include "tt_to_aig.hpp"

#include <tracy/Tracy.hpp>
#include <tbb/concurrent_hash_map.h>
#include <tbb/task_group.h>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <functional>

namespace bmm {

namespace {

struct DttCompare {
    static bool equal(const kitty::dynamic_truth_table& a, const kitty::dynamic_truth_table& b) {
        return a == b;
    }
    static size_t hash(const kitty::dynamic_truth_table& tt) {
        size_t h = 0;
        for (auto it = tt.begin(); it != tt.end(); ++it) {
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

using MemoMap = tbb::concurrent_hash_map<kitty::dynamic_truth_table, std::shared_ptr<NodeEntry>, DttCompare>;

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

    std::function<mockturtle::aig_network::signal(const kitty::dynamic_truth_table&, uint32_t)> build_aig_rec =
        [&](const kitty::dynamic_truth_table& current_tt, uint32_t var_idx) -> mockturtle::aig_network::signal {
        
        // 1. Const checks
        if (kitty::is_const0(current_tt)) {
            return net.get_constant(false);
        }
        if (kitty::is_const0(~current_tt)) {
            return net.get_constant(true);
        }

        // 2. Base case
        if (var_idx == 0) {
            if (kitty::get_bit(current_tt, 1)) {
                return pi_signals[0];
            } else {
                return !pi_signals[0];
            }
        }

        // 3. Memoization
        MemoMap::accessor acc;
        bool is_new = memo.insert(acc, current_tt);
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

        // Parallelize only if var_idx is high to prevent excessive TBB nesting and deadlock
        if (var_idx >= 5) {
            tbb::task_group tg;
            tg.run([&] { lo_signal = build_aig_rec(tt_lo, split_var); });
            hi_signal = build_aig_rec(tt_hi, split_var);
            tg.wait();
        } else {
            lo_signal = build_aig_rec(tt_lo, split_var);
            hi_signal = build_aig_rec(tt_hi, split_var);
        }

        auto s = pi_signals[split_var];
        auto term_hi = net.create_and(s, hi_signal);
        auto term_lo = net.create_and(!s, lo_signal);
        auto result_signal = net.create_or(term_hi, term_lo);

        entry->val = result_signal;
        entry->ready = true;
        return result_signal;
    };

    auto final_signal = build_aig_rec(tt.raw(), n);
    net.create_po(final_signal);

    return ok<Aig>(Aig(std::move(net)));
}

}  // namespace bmm
