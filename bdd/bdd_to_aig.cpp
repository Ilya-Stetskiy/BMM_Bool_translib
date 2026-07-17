#include "bdd_to_aig.hpp"
#include <tracy/Tracy.hpp>
#include <mockturtle/networks/aig.hpp>
#include <sylvan_obj.hpp>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cassert>
#include <stdexcept>

namespace bmm {

Result<Aig> bdd_to_aig(const Bdd& f) {
    ZoneScoped;
    const uint64_t COMP_BIT = 0x8000000000000000ULL;

    try {
        mockturtle::aig_network aig;
        
        auto const_one = aig.get_constant(true);
        auto const_zero = !const_one;

        uint32_t num_vars = f.n_vars();
        std::vector<mockturtle::aig_network::signal> pis(num_vars);
        for (uint32_t i = 0; i < num_vars; ++i) {
            pis[i] = aig.create_pi();
        }

        sylvan::Bdd f_syl = f.raw();
        
        if (f_syl.isZero()) {
            aig.create_po(const_zero);
            return ok(Aig(std::move(aig)));
        }
        if (f_syl.isOne()) {
            aig.create_po(const_one);
            return ok(Aig(std::move(aig)));
        }

        size_t dag_size = std::max<size_t>(f_syl.NodeCount(), 64);
        
        std::unordered_map<uint64_t, mockturtle::aig_network::signal> cache;
        cache.reserve(dag_size);
        std::unordered_set<uint64_t> visited;
        visited.reserve(dag_size);
        std::unordered_set<uint64_t> seen;
        seen.reserve(dag_size);
        std::vector<sylvan::Bdd> stack;
        stack.reserve(dag_size);

        sylvan::Bdd f_reg(f_syl.GetBDD() & ~COMP_BIT);
        stack.push_back(f_reg);
        seen.insert(f_reg.GetBDD());

        std::vector<sylvan::Bdd> post_order;
        post_order.reserve(dag_size);

        while (!stack.empty()) {
            sylvan::Bdd reg = stack.back();
            uint64_t reg_id = reg.GetBDD();

            if (visited.count(reg_id)) {
                stack.pop_back();
                continue;
            }

            if (reg.isZero() || reg.isOne()) {
                visited.insert(reg_id);
                stack.pop_back();
                continue;
            }

            sylvan::Bdd T = reg.Then();
            sylvan::Bdd E = reg.Else();
            
            sylvan::Bdd reg_T(T.GetBDD() & ~COMP_BIT);
            sylvan::Bdd reg_E(E.GetBDD() & ~COMP_BIT);
            
            uint64_t reg_T_id = reg_T.GetBDD();
            uint64_t reg_E_id = reg_E.GetBDD();

            bool T_ready = (T.isZero() || T.isOne() || visited.count(reg_T_id));
            bool E_ready = (E.isZero() || E.isOne() || visited.count(reg_E_id));

            if (T_ready && E_ready) {
                visited.insert(reg_id);
                post_order.push_back(reg);
                stack.pop_back();
            } else {
                if (!T_ready && !seen.count(reg_T_id)) {
                    seen.insert(reg_T_id);
                    stack.push_back(reg_T);
                }
                if (!E_ready && !seen.count(reg_E_id)) {
                    seen.insert(reg_E_id);
                    stack.push_back(reg_E);
                }
            }
        }

        auto get_child_signal = [&](const sylvan::Bdd& child) -> mockturtle::aig_network::signal {
            bool is_comp = (child.GetBDD() & COMP_BIT) != 0;
            sylvan::Bdd reg_child(child.GetBDD() & ~COMP_BIT);
            mockturtle::aig_network::signal base_sig;
            
            if (reg_child.isZero()) {
                base_sig = const_zero; 
            } else if (reg_child.isOne()) {
                base_sig = const_one;
            } else {
                auto it = cache.find(reg_child.GetBDD());
                if (it == cache.end()) {
                    throw std::logic_error("bdd_to_aig: узел не найден в кэше");
                }
                base_sig = it->second;
            }
            
            return is_comp ? !base_sig : base_sig;
        };

        for (const sylvan::Bdd& reg_node : post_order) {
            uint32_t var_idx = reg_node.TopVar();
            if (var_idx >= num_vars) {
                return fail<Aig>(ErrorCode::InvalidArgument, "bdd_to_aig: индекс BDD превышает n_vars()");
            }
            
            auto x_sig = pis[var_idx];
            auto hi_sig = get_child_signal(reg_node.Then());
            auto lo_sig = get_child_signal(reg_node.Else());
            
            auto n1 = aig.create_and(x_sig, hi_sig);
            auto n2 = aig.create_and(!x_sig, lo_sig);
            auto ite_sig = !aig.create_and(!n1, !n2);
            
            cache[reg_node.GetBDD()] = ite_sig;
        }

        aig.create_po(get_child_signal(f_syl));
        return ok(Aig(std::move(aig)));

    } catch (const std::bad_alloc&) {
        return fail<Aig>(ErrorCode::OutOfMemory, "bdd_to_aig: исчерпана память");
    } catch (const std::exception& e) {
        return fail<Aig>(ErrorCode::InvalidArgument, std::string("bdd_to_aig error: ") + e.what());
    }
}

} // namespace bmm