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

  // ДОБАВЛЕНО (унификация с остальными Aig-потребляющими функциями —
  // aig_to_bdd/aig_to_anf/aig_to_thr уже ловят bad_alloc): `gates` ниже
  // масштабируется размером входного Aig (net.num_gates()), а не n_vars() —
  // kMaxTruthTableVars ограничивает только n, не размер входной схемы.
  // ИЗВЕСТНОЕ ОГРАНИЧЕНИЕ: этот catch защищает только аллокации ДО
  // `#pragma omp parallel` ниже (в частности `gates`). `node_vals` внутри
  // omp-региона аллоцируется в каждом потоке независимо — bad_alloc там,
  // не пойманный ВНУТРИ того же потока/региона, по спецификации OpenMP не
  // гарантированно долетает до этого внешнего catch (в отличие от TBB,
  // которая явно пробрасывает исключения задач наружу) — реалистичный
  // необработанный крайний случай, если net_size окажется достаточно
  // большим. Полная защита потребовала бы per-thread try/catch с
  // передачей флага ошибки наружу через shared-переменную — не сделано в
  // рамках этой правки (только унификация политики исключений, не
  // отдельная задача про OpenMP-safety).
  try {

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
    // Второй раунд (устранение аллокаций напрямую, см. историю git и
    // aig/README.md §1.5): сеть разворачивается в плоский массив гейтов ОДИН
    // РАЗ до параллельного участка (topological order уже гарантирован:
    // mockturtle::aig_network хранит инвариант "индекс фаниина всегда меньше
    // индекса узла", foreach_gate обходит в порядке индекса — см. также
    // комментарий в bdd_to_tt.cpp про тот же инвариант). Внутри `#pragma omp
    // parallel` каждый поток заводит СВОЙ буфер значений узлов (node_vals)
    // ОДИН РАЗ ДО цикла по блокам — ноль аллокаций в горячем цикле.
    //
    // Третий раунд (K=10, эта правка): после устранения аллокаций ширину
    // блока СНАЧАЛА вернули к "естественным" K=6 (одно 64-битное слово) —
    // гипотеза "раз аллокаций нет, мельче блоки однозначно лучше" (aig/
    // README.md §1.5, TRANSLATION_MATRIX.md TODO) была явно оставлена
    // НЕПОДТВЕРЖДЁННОЙ, не проверенной. Проверено отдельным диагностическим
    // прогоном (verify/diag_aig_to_tt_k10.cpp, удалён после записи чисел) —
    // гипотеза НЕ подтвердилась в обратную сторону: широкий блок (K=10,
    // 1024 бита/вызов) в комбинации с allocation-free оказался БЫСТРЕЕ
    // узкого (K=6) на всех практически значимых размерах — n=16: 2.46x,
    // n=20: 2.25x, n=24: 1.52x (лучший режим каждого варианта, медиана 5
    // измерений, GitHub Actions runner, cross-check результатов K=6 vs K=10
    // — PASS на всех размерах). Проигрыш K=10 виден только на n=12
    // (0.004мс vs 0.006мс — доли микросекунды, шум диспетчеризации
    // OpenMP/малое число блоков перевешивает выгоду от более широкого слова,
    // не структурный эффект). Итог: K=10 — новое значение по умолчанию.
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

    // K=10 — см. комментарий выше (замер verify/diag_aig_to_tt_k10.cpp):
    // 1024 бита/вызов вместо 64, в комбинации с allocation-free быстрее на
    // всех практически значимых n.
    constexpr uint32_t K = 10;
    constexpr uint64_t kWordsPerBlock = 1ULL << (K - 6);  // 16 машинных слов на блок

    const uint64_t num_blocks = (n < K) ? 1ULL : (1ULL << (n - K));
    // Сколько 64-битных слов реально занимает результат целиком (совпадает
    // с фактическим размером аллокации tt.raw()) — при n < K вычисляется
    // ПОЛНЫЙ K-битный блок (kWordsPerBlock слов), но реально нужен только
    // этот префикс, остальное — избыточное повторение (тот же принцип, что
    // раньше был для n < 6, обобщённый на произвольный K).
    const uint64_t total_words = (n < 6) ? 1ULL : (1ULL << (n - 6));
    uint64_t* tt_data = &(*tt.raw().begin());

    #pragma omp parallel
    {
        // Буфер на поток заводится ОДИН раз здесь (не внутри omp for) —
        // переиспользуется для всех блоков, назначенных этому потоку, без
        // единой дополнительной аллокации в цикле ниже.
        std::vector<kitty::static_truth_table<K>> node_vals(net_size);
        kitty::static_truth_table<K> zero_tt;
        kitty::static_truth_table<K> one_tt = ~zero_tt;

        std::array<kitty::static_truth_table<K>, K> base_var_tts;
        for (uint32_t i = 0; i < std::min(n, K); ++i) {
            kitty::create_nth_var(base_var_tts[i], i);
        }

        #pragma omp for schedule(static)
        for (int64_t w = 0; w < static_cast<int64_t>(num_blocks); ++w) {
            node_vals[const_node] = zero_tt;

            for (uint32_t i = 0; i < std::min(n, K); ++i) {
                node_vals[pi_nodes[i]] = base_var_tts[i];
            }

            const uint64_t chunk_idx = static_cast<uint64_t>(w);
            for (uint32_t i = K; i < n; ++i) {
                bool bit = (chunk_idx >> (i - K)) & 1ULL;
                node_vals[pi_nodes[i]] = bit ? one_tt : zero_tt;
            }

            for (const auto& gate : gates) {
                auto val0 = gate.inv0 ? ~node_vals[gate.src0] : node_vals[gate.src0];
                auto val1 = gate.inv1 ? ~node_vals[gate.src1] : node_vals[gate.src1];
                node_vals[gate.dest] = val0 & val1;
            }

            auto res_tt = po_inv ? ~node_vals[po_node] : node_vals[po_node];
            // n >= K: этот блок целиком, kWordsPerBlock слов, на своё
            // законное место w-го блока. n < K: единственная (num_blocks=1)
            // "виртуальная" итерация считает полный K-битный блок, но
            // реально нужен только префикс в total_words < kWordsPerBlock
            // слов — писать за его пределы было бы переполнением буфера
            // tt.raw() (тот выделен под total_words слов, не kWordsPerBlock).
            const uint64_t words_to_write = (n >= K) ? kWordsPerBlock : total_words;
            std::copy_n(res_tt.begin(), words_to_write, tt_data + w * words_to_write);
        }
    }

    if (n < 6) {
        // Меньше 6 переменных -> результат уместился в младшие 2^n бит
        // одного 64-битного слова, но вычислен как будто есть все K
        // "слотов" (те, что сверх n, никогда не читаются ни одним гейтом,
        // поэтому не влияют на результат) — маскируем лишние повторы.
        *tt_data &= (1ULL << (1ULL << n)) - 1ULL;
    }

    return ok<TruthTable>(std::move(tt));

  } catch (const std::bad_alloc&) {
      return out_of_memory<TruthTable>("aig_to_tt");
  }
}

}  // namespace bmm
