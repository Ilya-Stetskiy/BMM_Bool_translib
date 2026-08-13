// verify/diag_aig_to_tt_k10.cpp — ВРЕМЕННЫЙ диагностический бинарь (не
// таргет ctest, не коммитится в CMakeLists.txt дольше, чем нужно для записи
// чисел — тот же принцип, что у уже удалённых diag_boundary_timing.cpp/
// diag_anf_dataset.cpp этой же сессии).
//
// Закрывает открытый вопрос из TRANSLATION_MATRIX.md/aig/README.md §1.5:
// после того как aig_to_tt стал allocation-free (плоский массив гейтов +
// персистентный per-thread буфер, см. aig/aig_to_tt.cpp), ширина блока была
// возвращена с K=10 к "естественным" K=6 — но гипотеза "комбинация
// allocation-free + widening (K=10) может быть ещё быстрее" не была
// проверена, только зафиксирована как открытая.
//
// Здесь — ТОТ ЖЕ allocation-free алгоритм, что в aig_to_tt.cpp, но с
// kitty::static_truth_table<10> вместо <6> (реализация продублирована, не
// параметризована шаблоном в самом aig_to_tt.cpp — этот файл временный,
// трогать production-код ради него не нужно), измеренный на ТОЙ ЖЕ машине,
// в ТОМ ЖЕ прогоне, что и текущий production aig_to_tt (K=6) — прямое
// сравнение, не через старые числа из README (другая машина/время).
//
// Методология — та же, что benchmarks/large_scale_bench.cpp::sweep для
// aig_to_tt: random_aig(n, n*2, seed), n={12,16,20,24}, однопоточно vs
// OpenMP-параллельно, медиана 5 измерений (benchmarks/scaling.hpp).

#include <bmm/aig/aig_to_tt.hpp>
#include <bmm/core/common.hpp>

#include "benchmarks/large_scale_generators.hpp"
#include "benchmarks/scaling.hpp"

#include <kitty/constructors.hpp>
#include <kitty/static_truth_table.hpp>

#include <mockturtle/networks/aig.hpp>

#include <omp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace bmm;
using namespace bmm::benchmarks;

namespace {

constexpr uint64_t kSeed = 20260719;
constexpr int kCorrectnessSamples = 30;

// Дословная копия allocation-free алгоритма из aig/aig_to_tt.cpp, K
// параметризован шаблоном (aig_to_tt.cpp сам не шаблонный — там жёстко K=6,
// см. комментарий в шапке файла).
template <uint32_t K>
Result<TruthTable> aig_to_tt_blockK(const Aig& aig) {
    const auto& net = aig.raw();
    if (net.num_pos() != 1) {
        return fail<TruthTable>(ErrorCode::Unsupported, "expected exactly one PO");
    }
    const uint32_t n = aig.n_vars();
    if (n > kMaxTruthTableVars) {
        return fail<TruthTable>(ErrorCode::TooManyVariables, "too many variables");
    }

    try {
        TruthTable tt(n);

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
            gates.push_back({dest,
                              static_cast<uint32_t>(net.node_to_index(net.get_node(fanins[0]))),
                              net.is_complemented(fanins[0]),
                              static_cast<uint32_t>(net.node_to_index(net.get_node(fanins[1]))),
                              net.is_complemented(fanins[1])});
        });

        mockturtle::aig_network::signal po_sig;
        net.foreach_po([&](auto signal) { po_sig = signal; });
        const uint32_t po_node = net.node_to_index(net.get_node(po_sig));
        const bool po_inv = net.is_complemented(po_sig);

        std::vector<uint32_t> pi_nodes(n);
        uint32_t pi_counter = 0;
        net.foreach_pi([&](auto node) { pi_nodes[pi_counter++] = net.node_to_index(node); });

        const uint32_t net_size = net.size();
        const uint32_t const_node = net.node_to_index(net.get_node(net.get_constant(false)));

        const uint64_t num_blocks = (n < K) ? 1ULL : (1ULL << (n - K));
        uint64_t* tt_data = &(*tt.raw().begin());
        // K может быть > 6 — блок тогда шире одного uint64_t, нужно писать
        // NumBlocks=2^(K-6) машинных слов на "виртуальный" блок, не одно.
        constexpr uint64_t kWordsPerBlock = (K > 6) ? (1ULL << (K - 6)) : 1ULL;

        #pragma omp parallel
        {
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
                std::copy(res_tt.begin(), res_tt.end(), tt_data + w * kWordsPerBlock);
            }
        }

        if (n < K) {
            const uint32_t words = static_cast<uint32_t>(kWordsPerBlock);
            if (n < 6) {
                tt_data[0] &= (1ULL << (1ULL << n)) - 1ULL;
            }
            (void)words;
        }

        return ok<TruthTable>(std::move(tt));
    } catch (const std::bad_alloc&) {
        return out_of_memory<TruthTable>("aig_to_tt_blockK");
    }
}

double now_ms_diff(std::chrono::steady_clock::time_point t0, std::chrono::steady_clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

bool sampled_equal(const TruthTable& a, const TruthTable& b, uint32_t n_vars, uint64_t seed) {
    std::mt19937_64 rng(seed);
    Assignment assignment(n_vars, false);
    for (int s = 0; s < kCorrectnessSamples; ++s) {
        for (uint32_t i = 0; i < n_vars; ++i) assignment[i] = (rng() & 1u) != 0;
        if (a.evaluate(assignment) != b.evaluate(assignment)) return false;
    }
    return true;
}

template <class Fn>
double median_single_vs_parallel(Fn&& translate, bool parallel, uint32_t n_threads_if_parallel) {
    const int default_threads = omp_get_max_threads();
    omp_set_num_threads(parallel ? n_threads_if_parallel : 1);
    std::vector<double> samples;
    samples.reserve(kMeasuredRuns);
    translate();  // прогрев
    for (int i = 0; i < kMeasuredRuns; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        translate();
        auto t1 = std::chrono::steady_clock::now();
        samples.push_back(now_ms_diff(t0, t1));
    }
    omp_set_num_threads(default_threads);
    return bmm::benchmarks::detail::median_of(std::move(samples));
}

}  // namespace

int main() {
    const uint32_t n_threads = static_cast<uint32_t>(omp_get_max_threads());
    std::printf("=== diag_aig_to_tt_k10: K=6 (production, aig_to_tt.hpp) vs K=10 (эксперимент) ===\n");
    std::printf("omp_get_max_threads() = %u\n\n", n_threads);

    for (uint32_t n : {12u, 16u, 20u, 24u}) {
        Aig input = random_aig(n, n * 2, kSeed);

        TruthTable k6_result(1), k10_result(1);
        bool k6_ok = false, k10_ok = false;

        double k6_single = median_single_vs_parallel(
            [&]() {
                auto r = aig_to_tt(input);
                k6_ok = is_ok(r);
                if (k6_ok) k6_result = value(r);
            },
            false, n_threads);
        double k6_parallel = median_single_vs_parallel(
            [&]() {
                auto r = aig_to_tt(input);
                if (is_ok(r)) k6_result = value(r);
            },
            true, n_threads);

        double k10_single = median_single_vs_parallel(
            [&]() {
                auto r = aig_to_tt_blockK<10>(input);
                k10_ok = is_ok(r);
                if (k10_ok) k10_result = value(r);
            },
            false, n_threads);
        double k10_parallel = median_single_vs_parallel(
            [&]() {
                auto r = aig_to_tt_blockK<10>(input);
                if (is_ok(r)) k10_result = value(r);
            },
            true, n_threads);

        bool cross_check = k6_ok && k10_ok && sampled_equal(k6_result, k10_result, n, kSeed);

        std::printf(
            "n=%-3u  K=6:  single=%9.3fмс parallel=%9.3fмс speedup=%5.2fx\n"
            "        K=10: single=%9.3fмс parallel=%9.3fмс speedup=%5.2fx\n"
            "        K10-vs-K6 (лучший режим каждого): %5.2fx   cross-check(K6==K10)=%s\n\n",
            n, k6_single, k6_parallel, k6_single / k6_parallel, k10_single, k10_parallel,
            k10_single / k10_parallel,
            std::min(k6_single, k6_parallel) / std::min(k10_single, k10_parallel),
            cross_check ? "PASS" : "FAIL");
        std::fflush(stdout);
    }

    return 0;
}
