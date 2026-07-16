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

// --- ХЕЛПЕРЫ ДЛЯ РАБОТЫ С СЫРЫМ SYLVAN API ---
// ИСПРАВЛЕНИЕ 1: sylvan_complement находится в namespace sylvan::
inline bool sylvan_is_comp(const sylvan::Bdd& n) {
    return (n.GetBDD() & sylvan::sylvan_complement) != 0;
}

inline sylvan::Bdd sylvan_regular(const sylvan::Bdd& n) {
    return sylvan_is_comp(n) ? !n : n;
}

Result<Aig> bdd_to_aig(const Bdd& f) {
    ZoneScoped;

    try {
        mockturtle::aig_network aig;
        
        // ИСПРАВЛЕНИЕ 2: используем публичный геттер raw() из common.hpp
        sylvan::Bdd f_syl = f.raw();
        
        if (f_syl.isTerminal()) {
            bool val = !sylvan_is_comp(f_syl);
            auto sig = aig.get_constant(val);
            aig.create_po(sig);
            return ok(Aig(std::move(aig)));
        }

        uint32_t num_vars = f.n_vars();
        std::vector<mockturtle::aig_network::signal> pis(num_vars);
        for (uint32_t i = 0; i < num_vars; ++i) {
            pis[i] = aig.create_pi();
        }

        // В C++ API Sylvan метод называется NodeCount() (с большой буквы)
        // Если не компилируется — попробуй nodeCount() или dagSize()
        int dag_size = f_syl.NodeCount();
        
        std::unordered_map<uint64_t, mockturtle::aig_network::signal> cache;
        cache.reserve(dag_size);
        
        std::unordered_set<uint64_t> visited;
        visited.reserve(dag_size);
        
        std::unordered_set<uint64_t> seen;
        seen.reserve(dag_size);

        std::vector<sylvan::Bdd> stack;
        stack.reserve(dag_size);
        
        sylvan::Bdd f_reg = sylvan_regular(f_syl);
        stack.push_back(f_syl);
        seen.insert(f_reg.GetBDD());

        std::vector<sylvan::Bdd> post_order;
        post_order.reserve(dag_size);

        // Итеративный Post-Order обход
        // Методы sylvan::Bdd (Then(), Else(), TopVar()) автоматически 
        // работают в контексте Lace — не нужен явный lace_call
        while (!stack.empty()) {
            sylvan::Bdd curr = stack.back();
            sylvan::Bdd reg = sylvan_regular(curr);
            uint64_t reg_id = reg.GetBDD();

            if (visited.count(reg_id)) {
                stack.pop_back();
                continue;
            }

            if (reg.isTerminal()) {
                visited.insert(reg_id);
                stack.pop_back();
                continue;
            }

            sylvan::Bdd T = reg.Then();
            sylvan::Bdd E = reg.Else();
            
            sylvan::Bdd reg_T = sylvan_regular(T);
            sylvan::Bdd reg_E = sylvan_regular(E);
            
            uint64_t reg_T_id = reg_T.GetBDD();
            uint64_t reg_E_id = reg_E.GetBDD();

            bool T_ready = reg_T.isTerminal() || visited.count(reg_T_id);
            bool E_ready = reg_E.isTerminal() || visited.count(reg_E_id);

            if (T_ready && E_ready) {
                visited.insert(reg_id);
                post_order.push_back(reg);
                stack.pop_back();
            } else {
                if (!T_ready && !seen.count(reg_T_id)) {
                    seen.insert(reg_T_id);
                    stack.push_back(T); 
                }
                if (!E_ready && !seen.count(reg_E_id)) {
                    seen.insert(reg_E_id);
                    stack.push_back(E);
                }
            }
        }

        auto get_child_signal = [&](const sylvan::Bdd& child) -> mockturtle::aig_network::signal {
            sylvan::Bdd reg_child = sylvan_regular(child);
            mockturtle::aig_network::signal base_sig;
            
            if (reg_child.isTerminal()) {
                base_sig = aig.get_constant(true); 
            } else {
                auto it = cache.find(reg_child.GetBDD());
                assert(it != cache.end() && "Логическая ошибка: узел не найден в кэше!");
                base_sig = it->second;
            }
            
            return sylvan_is_comp(child) ? !base_sig : base_sig;
        };

        for (const sylvan::Bdd& reg_node : post_order) {
            uint32_t var_idx = reg_node.TopVar();
            
            if (var_idx >= num_vars) {
                return fail<Aig>(ErrorCode::Unsupported, "bdd_to_aig: индекс BDD превышает n_vars()");
            }
            
            auto x_sig = pis[var_idx];
            auto hi_sig = get_child_signal(reg_node.Then());
            auto lo_sig = get_child_signal(reg_node.Else());
            
            auto ite_sig = aig.create_ite(x_sig, hi_sig, lo_sig);
            cache[reg_node.GetBDD()] = ite_sig;
        }

        auto root_sig = get_child_signal(f_syl);
        aig.create_po(root_sig);

        return ok(Aig(std::move(aig)));

    } catch (const std::bad_alloc&) {
        return fail<Aig>(ErrorCode::OutOfMemory, "bdd_to_aig: исчерпана память при построении AIG");
    } catch (const std::exception& e) {
        return fail<Aig>(ErrorCode::Unsupported, std::string("bdd_to_aig internal error: ") + e.what());
    }
}

} // namespace bmm
