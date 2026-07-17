#include "bdd_to_aig.hpp"
#include <tracy/Tracy.hpp>
#include <mockturtle/networks/aig.hpp>
#include <parallel_hashmap/phmap.h>
#include <sylvan_obj.hpp>
#include <vector>
#include <stdexcept>

namespace bmm {

constexpr uint64_t COMP_BIT = 0x8000000000000000ULL;

enum class NodeState : uint8_t {
    New = 0,
    Expanded = 1,
    Built = 2
};

// Компактная структура: signal (4 байта) + state (1 байт) + padding (3 байта) = 8 байт
struct NodeInfo {
    mockturtle::aig_network::signal sig;
    uint8_t state = static_cast<uint8_t>(NodeState::New);
};

// Компактный кадр стека: raw (8 байт) + phase (1 байт) + padding (7 байт) = 16 байт
struct Frame {
    uint64_t raw;
    uint8_t phase; // 0 = pre-order (раскрыть), 1 = post-order (построить)
};

Result<Aig> bdd_to_aig(const Bdd& f) {
    ZoneScoped;

    try {
        mockturtle::aig_network aig;
        const uint32_t num_vars = f.n_vars();

        std::vector<mockturtle::aig_network::signal> pis(num_vars);
        for (uint32_t i = 0; i < num_vars; ++i) {
            pis[i] = aig.create_pi();
        }

        const sylvan::Bdd f_syl = f.raw();

        if (f_syl.isZero()) {
            aig.create_po(aig.get_constant(false));
            return ok(Aig(std::move(aig)));
        }
        if (f_syl.isOne()) {
            aig.create_po(aig.get_constant(true));
            return ok(Aig(std::move(aig)));
        }

        const size_t dag_size = std::max<size_t>(f_syl.NodeCount(), 64);

        // phmap::flat_hash_map дает выигрыш 20-40% по сравнению со std::unordered_map
        phmap::flat_hash_map<uint64_t, NodeInfo> node_info;
        node_info.reserve(dag_size);

        std::vector<Frame> stack;
        stack.reserve(dag_size * 2);

        const uint64_t f_reg_raw = f_syl.GetBDD() & ~COMP_BIT;

        stack.push_back({f_reg_raw, 0});

        while (!stack.empty()) {
            Frame frame = stack.back();
            stack.pop_back();

            const uint64_t reg_raw = frame.raw & ~COMP_BIT;
            
            if (frame.phase == 1) { // Post-order: построение
                auto it = node_info.find(reg_raw);
                if (it == node_info.end() || it->second.state != static_cast<uint8_t>(NodeState::Expanded)) {
                    continue;
                }

                const sylvan::Bdd reg_node(reg_raw);
                const uint32_t var_idx = reg_node.TopVar();
                
                if (var_idx >= num_vars) {
                    return fail<Aig>(ErrorCode::InvalidArgument, "bdd_to_aig: индекс BDD превышает n_vars()");
                }

                const auto x_sig = pis[var_idx];
                const sylvan::Bdd hi_bdd = reg_node.Then();
                const sylvan::Bdd lo_bdd = reg_node.Else();

                auto get_sig = [&](const sylvan::Bdd& child_bdd) {
                    const uint64_t c_raw = child_bdd.GetBDD();
                    const bool c_comp = (c_raw & COMP_BIT) != 0;
                    const uint64_t c_reg = c_raw & ~COMP_BIT;
                    
                    const sylvan::Bdd c_reg_node(c_reg);
                    
                    if (c_reg_node.isZero()) {
                        return c_comp ? !aig.get_constant(false) : aig.get_constant(false);
                    }
                    if (c_reg_node.isOne()) {
                        return c_comp ? !aig.get_constant(true) : aig.get_constant(true);
                    }
                    
                    auto child_it = node_info.find(c_reg);
                    if (child_it == node_info.end() || child_it->second.state != static_cast<uint8_t>(NodeState::Built)) {
                        throw std::logic_error("bdd_to_aig: child node not built (invariant violated)");
                    }
                    
                    const mockturtle::aig_network::signal s = child_it->second.sig;
                    return c_comp ? !s : s;
                };

                it->second.sig = aig.create_ite(x_sig, get_sig(hi_bdd), get_sig(lo_bdd));
                it->second.state = static_cast<uint8_t>(NodeState::Built);

            } else { // Pre-order: раскрытие
                auto [it_new, inserted] = node_info.try_emplace(reg_raw);
                
                if (!inserted) {
                    continue;
                }

                it_new->second.state = static_cast<uint8_t>(NodeState::Expanded);

                stack.push_back({reg_raw, 1});

                const sylvan::Bdd reg_node(reg_raw);
                const sylvan::Bdd T = reg_node.Then();
                const sylvan::Bdd E = reg_node.Else();
                
                const uint64_t T_reg = T.GetBDD() & ~COMP_BIT;
                const uint64_t E_reg = E.GetBDD() & ~COMP_BIT;

                if (!T.isZero() && !T.isOne()) {
                    stack.push_back({T_reg, 0});
                }
                if (!E.isZero() && !E.isOne()) {
                    stack.push_back({E_reg, 0});
                }
            }
        }

        const auto it = node_info.find(f_reg_raw);
        if (it == node_info.end() || it->second.state != static_cast<uint8_t>(NodeState::Built)) {
            return fail<Aig>(ErrorCode::InvalidArgument, "bdd_to_aig: внутренняя ошибка, корень не построен");
        }
        
        mockturtle::aig_network::signal root_sig = it->second.sig;
        const bool f_is_comp = (f_syl.GetBDD() & COMP_BIT) != 0;
        if (f_is_comp) {
            root_sig = !root_sig;
        }

        aig.create_po(root_sig);
        return ok(Aig(std::move(aig)));

    } catch (const std::bad_alloc&) {
        return fail<Aig>(ErrorCode::OutOfMemory, "bdd_to_aig: исчерпана память");
    } catch (const std::exception& e) {
        return fail<Aig>(ErrorCode::InvalidArgument, std::string("bdd_to_aig error: ") + e.what());
    }
}

} // namespace bmm