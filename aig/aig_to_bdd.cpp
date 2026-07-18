#include "aig_to_bdd.hpp"

#include <tracy/Tracy.hpp>
#include <vector>
#include <array>

namespace bmm {

Result<Bdd> aig_to_bdd(const Aig& aig) {
    ZoneScoped;
    const auto& net = aig.raw();
    if (net.num_pos() != 1) {
        return fail<Bdd>(ErrorCode::Unsupported, "aig_to_bdd: ожидается ровно один PO");
    }

  try {
    std::vector<sylvan::Bdd> node_bdds(net.size(), sylvan::Bdd::bddZero());
    
    // Constant false node
    node_bdds[net.node_to_index(net.get_node(net.get_constant(false)))] = sylvan::Bdd::bddZero();

    // Map PIs
    uint32_t pi_idx = 0;
    net.foreach_pi([&](auto node) {
        node_bdds[net.node_to_index(node)] = sylvan::Bdd::bddVar(pi_idx++);
    });

    // Traverse gates in topological order
    net.foreach_gate([&](auto node) {
        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        net.foreach_fanin(node, [&](auto signal) { fanins[k++] = signal; });

        auto get_signal_bdd = [&](mockturtle::aig_network::signal s) {
            auto node_idx = net.node_to_index(net.get_node(s));
            auto bdd = node_bdds[node_idx];
            if (net.is_complemented(s)) {
                return !bdd;
            }
            return bdd;
        };

        auto bdd0 = get_signal_bdd(fanins[0]);
        auto bdd1 = get_signal_bdd(fanins[1]);
        node_bdds[net.node_to_index(node)] = bdd0 & bdd1;
    });

    // PO signal
    mockturtle::aig_network::signal po_sig;
    net.foreach_po([&](auto signal) { po_sig = signal; });

    auto po_node_idx = net.node_to_index(net.get_node(po_sig));
    auto final_bdd = node_bdds[po_node_idx];
    if (net.is_complemented(po_sig)) {
        final_bdd = !final_bdd;
    }

    return ok<Bdd>(Bdd(final_bdd, aig.n_vars()));

  } catch (const std::bad_alloc&) {
      // ДОБАВЛЕНО: тот же класс риска, что и у anf_to_bdd (aig/README.md,
      // находка про взрыв BDD на "разнесённых" по индексу парах переменных —
      // см. anf/README.md §5.2) — здесь это никогда целенаправленно не
      // проверялось на bad_alloc в чистом виде (Sylvan/Lace обычно падает
      // через abort(), не бросает исключение, но нет причины не поймать и
      // bad_alloc там, где он всё же может прийти — например, из самого
      // std::vector<sylvan::Bdd> node_bdds на очень большой сети).
      return fail<Bdd>(ErrorCode::OutOfMemory, "aig_to_bdd: исчерпана память");
  }
}

}  // namespace bmm
