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

    // ИСПРАВЛЕНО (найдено benchmarks/large_scale_bench.cpp: параллельная
    // версия при n=12..24 стабильно давала speedup~1.00x, то есть НИКАКОГО
    // выигрыша от OpenMP несмотря на честно независимые по данным блоки).
    // Причина — не гонки/локи (их тут нет вовсе), а фиксированная цена
    // ОДНОГО вызова mockturtle::simulate(): внутри он аллоцирует
    // node_map (make_shared<vector<SimulationType>>(net.size())) и, что
    // важнее, ОТДЕЛЬНЫЙ vector<SimulationType> fanin_values НА КАЖДЫЙ гейт
    // сети (см. algorithms/simulation.hpp) — то есть на каждый блок
    // приходится ~net.size() мелких куч-аллокаций через один и тот же
    // glibc-аллокатор. При n=24 и блоке в 6 "свободных" переменных
    // num_blocks=2^18=262144, а полезная работа на блок — единицы AND/XOR
    // над 64-битным словом (наносекунды) — итог: время съедается malloc/free
    // и fork-join OpenMP, а не вычислениями, отсюда и отсутствие ускорения.
    //
    // Фикс — увеличить ширину блока (число "свободных" переменных на один
    // вызов simulate) с 6 до kBlockVars=10: тот же общий объём полезной
    // работы (то же суммарное число гейт-операций по всей truth table), но
    // в 2^(10-6)=16 раз МЕНЬШЕ вызовов simulate(), а значит в 16 раз меньше
    // аллокаций node_map/fanin_values на весь запуск. kitty::static_truth_table
    // для NumVars>6 хранит std::array<uint64_t, NumBlocks> с обычными
    // begin()/end() (проверено в kitty/static_truth_table.hpp) — копируем
    // все NumBlocks слов блока разом через std::copy вместо ручного побитового
    // разбора одного uint64_t, как было раньше.
    constexpr uint32_t kBlockVars = 10;

    if (n < kBlockVars) {
        struct custom_block_simulator {
            custom_block_simulator() = default;
            kitty::static_truth_table<kBlockVars> compute_constant(bool value) const {
                kitty::static_truth_table<kBlockVars> t;
                return value ? ~t : t;
            }
            kitty::static_truth_table<kBlockVars> compute_pi(uint32_t index) const {
                kitty::static_truth_table<kBlockVars> t;
                if (index < kBlockVars) {
                    kitty::create_nth_var(t, index);
                }
                return t;
            }
            kitty::static_truth_table<kBlockVars> compute_not(kitty::static_truth_table<kBlockVars> const& value) const {
                return ~value;
            }
        };

        custom_block_simulator sim;
        auto po_values = mockturtle::simulate<kitty::static_truth_table<kBlockVars>>(net, sim);
        for (uint64_t i = 0; i < (1ULL << n); ++i) {
            if (kitty::get_bit(po_values[0], i)) {
                kitty::set_bit(tt.raw(), i);
            }
        }
    } else {
        const uint64_t num_blocks = 1ULL << (n - kBlockVars);
        constexpr uint64_t kWordsPerBlock = (1ULL << kBlockVars) / 64;
        uint64_t* tt_data = &(*tt.raw().begin());

        struct custom_block_simulator {
            custom_block_simulator(uint64_t chunk_idx) : chunk_idx(chunk_idx) {}
            kitty::static_truth_table<kBlockVars> compute_constant(bool value) const {
                kitty::static_truth_table<kBlockVars> t;
                return value ? ~t : t;
            }
            kitty::static_truth_table<kBlockVars> compute_pi(uint32_t index) const {
                kitty::static_truth_table<kBlockVars> t;
                if (index < kBlockVars) {
                    kitty::create_nth_var(t, index);
                } else {
                    if ((chunk_idx >> (index - kBlockVars)) & 1ULL) {
                        t = ~t;
                    }
                }
                return t;
            }
            kitty::static_truth_table<kBlockVars> compute_not(kitty::static_truth_table<kBlockVars> const& value) const {
                return ~value;
            }
            uint64_t chunk_idx;
        };

        #pragma omp parallel for schedule(static)
        for (int64_t w = 0; w < static_cast<int64_t>(num_blocks); ++w) {
            custom_block_simulator sim(static_cast<uint64_t>(w));
            auto po_values = mockturtle::simulate<kitty::static_truth_table<kBlockVars>>(net, sim);
            std::copy(po_values[0].begin(), po_values[0].end(), tt_data + w * kWordsPerBlock);
        }
    }

    return ok<TruthTable>(std::move(tt));
}

}  // namespace bmm
