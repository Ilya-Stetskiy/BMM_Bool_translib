#include "aig_to_tt.hpp"

#include <tracy/Tracy.hpp>
#include <vector>
#include <array>
#include <omp.h>

namespace bmm {

Result<TruthTable> aig_to_tt(const Aig& aig) {
    ZoneScoped;
    const auto& net = aig.raw();
    if (net.num_pos() != 1) {
        return fail<TruthTable>(ErrorCode::Unsupported, "aig_to_tt: ожидается ровно один PO");
    }
    const uint32_t n = aig.n_vars();
    if (n > kMaxTruthTableVars) {
        return fail<TruthTable>(ErrorCode::TooManyVariables, "aig_to_tt: слишком много переменных");
    }

    TruthTable tt(n);

    if (n < 6) {
        struct custom_block_simulator {
            custom_block_simulator() = default;
            kitty::static_truth_table<6> compute_constant(bool value) const {
                kitty::static_truth_table<6> t;
                return value ? ~t : t;
            }
            kitty::static_truth_table<6> compute_pi(uint32_t index) const {
                kitty::static_truth_table<6> t;
                if (index < 6) {
                    kitty::create_nth_var(t, index);
                }
                return t;
            }
            kitty::static_truth_table<6> compute_not(kitty::static_truth_table<6> const& value) const {
                return ~value;
            }
        };

        custom_block_simulator sim;
        auto po_values = mockturtle::simulate<kitty::static_truth_table<6>>(net, sim);
        uint64_t simulated_bits = *po_values[0].begin();
        for (uint64_t i = 0; i < (1ULL << n); ++i) {
            if ((simulated_bits >> i) & 1ULL) {
                kitty::set_bit(tt.raw(), i);
            }
        }
    } else {
        const uint64_t num_blocks = 1ULL << (n - 6);
        uint64_t* tt_data = &(*tt.raw().begin());

        struct custom_block_simulator {
            custom_block_simulator(uint64_t chunk_idx) : chunk_idx(chunk_idx) {}
            kitty::static_truth_table<6> compute_constant(bool value) const {
                kitty::static_truth_table<6> t;
                return value ? ~t : t;
            }
            kitty::static_truth_table<6> compute_pi(uint32_t index) const {
                kitty::static_truth_table<6> t;
                if (index < 6) {
                    kitty::create_nth_var(t, index);
                } else {
                    if ((chunk_idx >> (index - 6)) & 1ULL) {
                        t = ~t;
                    }
                }
                return t;
            }
            kitty::static_truth_table<6> compute_not(kitty::static_truth_table<6> const& value) const {
                return ~value;
            }
            uint64_t chunk_idx;
        };

        #pragma omp parallel for schedule(static)
        for (int64_t w = 0; w < static_cast<int64_t>(num_blocks); ++w) {
            custom_block_simulator sim(static_cast<uint64_t>(w));
            auto po_values = mockturtle::simulate<kitty::static_truth_table<6>>(net, sim);
            tt_data[w] = *po_values[0].begin();
        }
    }

    return ok<TruthTable>(std::move(tt));
}

}  // namespace bmm
