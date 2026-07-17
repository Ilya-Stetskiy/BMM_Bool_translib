#include "bdd_to_aig.hpp"
#include <tracy/Tracy.hpp>
#include <mockturtle/networks/aig.hpp>
#include <sylvan_obj.hpp>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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
        
        // Увеличиваем запас для стека, так как теперь допускаются временные дубликаты
        std::vector<sylvan::Bdd> stack;
        stack.reserve(dag_size * 2);
        
        std::unordered_set<uint64_t> visited;
        visited.reserve(dag_size);

        sylvan::Bdd f_reg(f_syl.GetBDD() & ~COMP_BIT);
        stack.push_back(f_reg);

        std::vector<sylvan::Bdd> post_order;
        post_order.reserve(dag_size);

        // НАДЁЖНЫЙ итеративный Post-Order обход без риска бесконечного цикла
        while (!stack.empty()) {
            sylvan::Bdd curr = stack.back();
            uint64_t curr_id = curr.GetBDD();

            // Если узел уже полностью обработан, просто убираем его дубликат из стека
            if (visited.count(curr_id)) {
                stack.pop_back();
                continue;
            }

            // Терминалы обрабатываем сразу
            if (curr.isZero() || curr.isOne()) {
                visited.insert(curr_id);
                post_order.push_back(curr);
                stack.pop_back();
                continue;
            }

            sylvan::Bdd T = curr.Then();
            sylvan::Bdd E = curr.Else();
            
            uint64_t T_reg_id = T.GetBDD() & ~COMP_BIT;
            uint64_t E_reg_id = E.GetBDD() & ~COMP_BIT;

            bool T_visited = T.isZero() || T.isOne() || visited.count(T_reg_id);
            bool E_visited = E.isZero() || E.isOne() || visited.count(E_reg_id);

            if (T_visited && E_visited) {
                // Оба ребёнка обработаны, текущий узел готов
                visited.insert(curr_id);
                post_order.push_back(curr);
                stack.pop_back();
            } else {
                // Пушим непосещённых детей. 
                // Дубликаты допустимы: они будут безопасно проигнорированы 
                // проверкой visited.count() при их извлечении из стека.
                if (!T_visited) {
                    stack.push_back(sylvan::Bdd(T_reg_id));
                }
                if (!E_visited) {
                    stack.push_back(sylvan::Bdd(E_reg_id));
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
            
            // Надёжная ручная сборка ITE: f = ~( ~(x & hi) & ~(~x & lo) )
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