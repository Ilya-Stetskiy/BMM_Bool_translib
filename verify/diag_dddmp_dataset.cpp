// Временный диагностический бинарь — проверить benchmarks/dddmp_loader.hpp
// НАИЛУЧШИМ доступным способом: benchmarks/data/iis-nsk/bdd-bench содержит
// готовые DDDMP BDD для тех же самых схем EPFL, что уже используются в
// verify/real_datasets_tests.cpp (EPFL__ctrl.blif.bdd, EPFL__int2float.blif.
// bdd, EPFL__cavlc.blif.bdd, EPFL__router.blif.bdd <-> benchmarks/data/epfl/
// random_control/{ctrl,int2float,cavlc,router}.aig) — независимая, третьей
// стороной посчитанная контрольная точка для aig_to_bdd: загружаем AIG,
// строим Bdd через aig_to_bdd (наш код), отдельно загружаем DDDMP-эталон
// той же схемы через мост, сравниваем через evaluate() (напрямую, БЕЗ
// verify/chain_utils.hpp — тот тянет core/anf_repr.hpp/BRiAl, чей
// bundled cudd.h конфликтует с системным cudd.h, который нужен здесь для
// dddmp_loader.hpp, см. предупреждение в шапке того файла; здесь он и не
// нужен — Aig::evaluate()/Bdd::evaluate() уже есть в core/common.hpp без
// какого-либо участия Anf/BRiAl) — НЕ полагаясь при этом ни на что, кроме
// самого сравнения (обе стороны получены независимо: одна — трансляцией
// внутри проекта, другая — готовым файлом от внешнего источника).

#include "aig/aig_to_bdd.hpp"
#include "benchmarks/dddmp_loader.hpp"

#include <lorina/aiger.hpp>
#include <mockturtle/io/aiger_reader.hpp>
#include <mockturtle/networks/aig.hpp>

#include <sylvan_obj.hpp>

#include <array>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

using namespace bmm;
using namespace bmm::benchmarks;

namespace {

// Тот же код, что verify/real_datasets_tests.cpp (extract_single_output/
// load_epfl_single_output) — не вынесен в общий заголовок, продублирован
// здесь как временная диагностика, см. аналогичный прецедент в этой же
// сессии (verify/diag_router_bdd.cpp).
std::optional<mockturtle::aig_network> extract_single_output(const mockturtle::aig_network& src,
                                                               uint32_t po_index) {
    if (po_index >= src.num_pos()) return std::nullopt;
    mockturtle::aig_network dest;
    std::unordered_map<mockturtle::aig_network::node, mockturtle::aig_network::signal> node_map;
    node_map[src.get_node(src.get_constant(false))] = dest.get_constant(false);
    src.foreach_pi([&](auto n) { node_map[n] = dest.create_pi(); });
    src.foreach_gate([&](auto n) {
        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        src.foreach_fanin(n, [&](auto s) { fanins[k++] = s; });
        auto get_dest_signal = [&](mockturtle::aig_network::signal s) {
            auto ds = node_map.at(src.get_node(s));
            return src.is_complemented(s) ? !ds : ds;
        };
        node_map[n] = dest.create_and(get_dest_signal(fanins[0]), get_dest_signal(fanins[1]));
    });
    uint32_t idx = 0;
    std::optional<mockturtle::aig_network::signal> po_signal;
    src.foreach_po([&](auto s) {
        if (idx == po_index) {
            auto ds = node_map.at(src.get_node(s));
            po_signal = src.is_complemented(s) ? !ds : ds;
        }
        ++idx;
    });
    if (!po_signal) return std::nullopt;
    dest.create_po(*po_signal);
    return dest;
}

std::optional<Aig> load_epfl_single_output(const std::string& path, uint32_t po_index) {
    mockturtle::aig_network full;
    if (lorina::read_aiger(path, mockturtle::aiger_reader(full)) != lorina::return_code::success)
        return std::nullopt;
    auto extracted = extract_single_output(full, po_index);
    if (!extracted) return std::nullopt;
    return Aig(std::move(*extracted));
}

bool compare(const Aig& a, const Bdd& b, bool exhaustive) {
    uint32_t n = a.n_vars();
    if (n != b.n_vars()) return false;
    Assignment assignment(n, false);
    if (exhaustive) {
        uint64_t rows = uint64_t{1} << n;
        for (uint64_t idx = 0; idx < rows; ++idx) {
            for (uint32_t i = 0; i < n; ++i) assignment[i] = (idx >> i) & 1u;
            if (a.evaluate(assignment) != b.evaluate(assignment)) return false;
        }
        return true;
    }
    std::mt19937_64 rng(20260719);
    std::uniform_int_distribution<uint64_t> dist(0, (n >= 64) ? UINT64_MAX : ((uint64_t{1} << n) - 1));
    for (uint64_t s = 0; s < 5000; ++s) {
        uint64_t idx = dist(rng);
        for (uint32_t i = 0; i < n; ++i) assignment[i] = (idx >> i) & 1u;
        if (a.evaluate(assignment) != b.evaluate(assignment)) return false;
    }
    return true;
}

void cross_check(const std::string& aig_path, const std::string& dddmp_path, bool exhaustive) {
    std::printf("== %s vs %s ==\n", aig_path.c_str(), dddmp_path.c_str());
    std::fflush(stdout);

    auto aig = load_epfl_single_output(aig_path, 0);
    if (!aig) {
        std::printf("FAIL: не удалось загрузить %s\n", aig_path.c_str());
        std::fflush(stdout);
        return;
    }
    auto our_bdd = aig_to_bdd(*aig);
    if (!is_ok(our_bdd)) {
        std::printf("FAIL aig_to_bdd: %s\n", error(our_bdd).message.c_str());
        std::fflush(stdout);
        return;
    }
    std::printf("PASS aig_to_bdd: n_vars=%u, NodeCount=%zu\n", aig->n_vars(),
                static_cast<size_t>(value(our_bdd).raw().NodeCount()));
    std::fflush(stdout);

    auto ref_bdd = load_dddmp_bdd(dddmp_path, 0);
    if (!ref_bdd) {
        std::printf("FAIL: не удалось загрузить %s (root 0)\n", dddmp_path.c_str());
        std::fflush(stdout);
        return;
    }
    std::printf("PASS load_dddmp_bdd: n_vars=%u, NodeCount=%zu\n", ref_bdd->n_vars(),
                static_cast<size_t>(ref_bdd->raw().NodeCount()));
    std::fflush(stdout);

    bool eq = compare(*aig, *ref_bdd, exhaustive);
    std::printf("%s независимая сверка aig_to_bdd против стороннего DDDMP-эталона (%s)\n",
                eq ? "PASS" : "FAIL", exhaustive ? "перебор" : "выборка 5000 точек");
    std::printf("\n");
    std::fflush(stdout);
}

VOID_TASK_0(main_task) {
    sylvan::sylvan_set_sizes(1LL << 22, 1LL << 26, 1LL << 18, 1LL << 22);
    sylvan::sylvan_init_package();
    sylvan::sylvan_init_bdd();

    cross_check("benchmarks/data/epfl/random_control/ctrl.aig",
                "benchmarks/data/iis-nsk/bdd-bench/EPFL__ctrl.blif.bdd", true);
    cross_check("benchmarks/data/epfl/random_control/int2float.aig",
                "benchmarks/data/iis-nsk/bdd-bench/EPFL__int2float.blif.bdd", true);
    cross_check("benchmarks/data/epfl/random_control/cavlc.aig",
                "benchmarks/data/iis-nsk/bdd-bench/EPFL__cavlc.blif.bdd", true);
    cross_check("benchmarks/data/epfl/random_control/router.aig",
                "benchmarks/data/iis-nsk/bdd-bench/EPFL__router.blif.bdd", false);

    sylvan::sylvan_quit();
}

}  // namespace

int main() {
    const size_t deque_size = 1ULL << 21;
    lace_start(0, deque_size);
    RUN(main_task);
    lace_stop();
    return 0;
}
