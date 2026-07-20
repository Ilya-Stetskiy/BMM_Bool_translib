#include "aig_to_tt.hpp"

#include <tracy/Tracy.hpp>
#include <vector>
#include <array>
#include <algorithm>
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

    // ИСПРАВЛЕНО дважды на одну и ту же находку (benchmarks/large_scale_bench.cpp:
    // параллельная версия при n=12..24 стабильно давала speedup~1.00x —
    // независимые по данным блоки, но НИКАКОГО выигрыша от OpenMP). Корень:
    // не гонки/локи (их тут нет вовсе), а фиксированная цена ОДНОГО вызова
    // mockturtle::simulate() — внутри он аллоцирует node_map
    // (make_shared<vector<SimulationType>>(net.size())) и ОТДЕЛЬНЫЙ
    // vector<SimulationType> fanin_values НА КАЖДЫЙ гейт сети на каждый
    // вызов (см. algorithms/simulation.hpp) — при num_blocks=2^18 (n=24) это
    // сотни тысяч мелких куч-аллокаций, съедающих всё время при том, что
    // полезная работа на блок — единицы AND/XOR над 64-битным словом.
    //
    // Первая попытка фикса (в истории git) расширяла блок с 6 до 10
    // "свободных" переменных — в 16 раз меньше вызовов simulate(), а значит
    // меньше аллокаций, но не ноль. Найдено на независимой студенческой
    // ветке origin/aig (student2, коммит 2db40e7 "changes in aig_to_anf and
    // aig_to_tt without parallel") — идея адаптирована и проверена заново
    // (сам код скопирован не был; в том же коммите были отдельные, не
    // относящиеся сюда изменения aig_to_anf, которые сюда не переносим):
    // устранить аллокации НАПРЯМУЮ, а не реже их платить. Сеть разворачивается
    // в плоский массив гейтов ОДИН РАЗ до параллельного участка (topological
    // order уже гарантирован: mockturtle::aig_network хранит инвариант
    // "индекс фаниина всегда меньше индекса узла", foreach_gate обходит в
    // порядке индекса — см. также комментарий в bdd_to_tt.cpp про тот же
    // инвариант). Внутри `#pragma omp parallel` каждый поток заводит СВОЙ
    // буфер значений узлов (node_vals) ОДИН РАЗ ДО цикла по блокам — ноль
    // аллокаций в горячем цикле вместо просто "меньше". Раз аллокации
    // перестали быть узким местом, ширина блока возвращена к естественной
    // (одно 64-битное слово truth table) — мельче и лучше сбалансированные
    // между потоками единицы работы, без платы аллокациями за это.
    struct FlatGate {
        uint32_t dest;
        uint32_t src0;
        bool inv0;
        uint32_t src1;
        bool inv1;
    };

    std::vector<FlatGate> gates;
    gates.reserve(net.num_gates());
    net.foreach_gate([&](auto node) {
        uint32_t dest = net.node_to_index(node);
        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        net.foreach_fanin(node, [&](auto signal) { fanins[k++] = signal; });
        gates.push_back({
            dest,
            static_cast<uint32_t>(net.node_to_index(net.get_node(fanins[0]))),
            net.is_complemented(fanins[0]),
            static_cast<uint32_t>(net.node_to_index(net.get_node(fanins[1]))),
            net.is_complemented(fanins[1])
        });
    });

    mockturtle::aig_network::signal po_sig;
    net.foreach_po([&](auto signal) { po_sig = signal; });
    const uint32_t po_node = net.node_to_index(net.get_node(po_sig));
    const bool po_inv = net.is_complemented(po_sig);

    std::vector<uint32_t> pi_nodes(n);
    uint32_t pi_counter = 0;
    net.foreach_pi([&](auto node) {
        pi_nodes[pi_counter++] = net.node_to_index(node);
    });

    const uint32_t net_size = net.size();
    const uint32_t const_node = net.node_to_index(net.get_node(net.get_constant(false)));

    const uint64_t num_blocks = (n < 6) ? 1ULL : (1ULL << (n - 6));
    uint64_t* tt_data = &(*tt.raw().begin());

    #pragma omp parallel
    {
        // Буфер на поток заводится ОДИН раз здесь (не внутри omp for) —
        // переиспользуется для всех блоков, назначенных этому потоку, без
        // единой дополнительной аллокации в цикле ниже.
        std::vector<kitty::static_truth_table<6>> node_vals(net_size);
        kitty::static_truth_table<6> zero_tt;
        kitty::static_truth_table<6> one_tt = ~zero_tt;

        std::array<kitty::static_truth_table<6>, 6> base_var_tts;
        for (uint32_t i = 0; i < std::min(n, 6u); ++i) {
            kitty::create_nth_var(base_var_tts[i], i);
        }

        #pragma omp for schedule(static)
        for (int64_t w = 0; w < static_cast<int64_t>(num_blocks); ++w) {
            node_vals[const_node] = zero_tt;

            for (uint32_t i = 0; i < std::min(n, 6u); ++i) {
                node_vals[pi_nodes[i]] = base_var_tts[i];
            }

            const uint64_t chunk_idx = static_cast<uint64_t>(w);
            for (uint32_t i = 6; i < n; ++i) {
                bool bit = (chunk_idx >> (i - 6)) & 1ULL;
                node_vals[pi_nodes[i]] = bit ? one_tt : zero_tt;
            }

            for (const auto& gate : gates) {
                auto val0 = gate.inv0 ? ~node_vals[gate.src0] : node_vals[gate.src0];
                auto val1 = gate.inv1 ? ~node_vals[gate.src1] : node_vals[gate.src1];
                node_vals[gate.dest] = val0 & val1;
            }

            auto res_tt = po_inv ? ~node_vals[po_node] : node_vals[po_node];
            tt_data[w] = *res_tt.begin();
        }
    }

    if (n < 6) {
        // Меньше 6 переменных -> результат уместился в младшие 2^n бит
        // одного 64-битного слова, но вычислен как будто есть все 6
        // "слотов" (те, что сверх n, никогда не читаются ни одним гейтом,
        // поэтому не влияют на результат) — маскируем лишние повторы.
        *tt_data &= (1ULL << (1ULL << n)) - 1ULL;
    }

    return ok<TruthTable>(std::move(tt));
}

}  // namespace bmm
