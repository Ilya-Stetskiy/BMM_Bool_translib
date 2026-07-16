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
    // ВАЖНО: Макрос профилировщика должен быть первой строкой в scope!
    // Он замерит время выполнения всей функции вплоть до её возврата.
    ZoneScoped;

    // КОНТРАКТ 2а: Graceful degradation. Перехват комбинаторного взрыва памяти.
    try {
        mockturtle::aig_network aig;
        
        // КОНТРАКТ 1: Надежная проверка терминала. 
        if (f.isTerminal()) {
            bool val = !f.isComp();
            auto sig = aig.get_constant(val);
            aig.create_po(sig);
            return ok(Aig(std::move(aig)));
        }

        uint32_t num_vars = f.n_vars();
        std::vector<mockturtle::aig_network::signal> pis(num_vars);
        for (uint32_t i = 0; i < num_vars; ++i) {
            pis[i] = aig.create_pi(); // LSB_FIRST гарантируется порядком создания PI
        }

        // Оценка размера для предотвращения реаллокаций и фрагментации кучи.
        int dag_size = f.nodecount(); // В Sylvan метод называется nodecount()
        
        std::unordered_map<uint64_t, mockturtle::aig_network::signal> cache;
        cache.reserve(dag_size);
        
        std::unordered_set<uint64_t> visited;
        visited.reserve(dag_size);
        
        // 'seen' предотвращает добавление одного и того же узла в стек многократно
        std::unordered_set<uint64_t> seen;
        seen.reserve(dag_size);

        std::vector<sylvan::Bdd> stack;
        stack.reserve(dag_size);
        
        sylvan::Bdd f_reg = f.isComp() ? !f : f;
        stack.push_back(f);
        seen.insert(f_reg.GetMTBDD());

        std::vector<sylvan::Bdd> post_order;
        post_order.reserve(dag_size);

        // КОНТРАКТ 3: Строго однопоточный итеративный Post-Order обход.
        while (!stack.empty()) {
            sylvan::Bdd curr = stack.back();
            sylvan::Bdd reg = curr.isComp() ? !curr : curr;
            uint64_t reg_id = reg.GetMTBDD();

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
            
            sylvan::Bdd reg_T = T.isComp() ? !T : T;
            sylvan::Bdd reg_E = E.isComp() ? !E : E;
            
            uint64_t reg_T_id = reg_T.GetMTBDD();
            uint64_t reg_E_id = reg_E.GetMTBDD();

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

        // Лямбда для безопасного извлечения сигнала
        auto get_child_signal = [&](const sylvan::Bdd& child) -> mockturtle::aig_network::signal {
            sylvan::Bdd reg_child = child.isComp() ? !child : child;
            mockturtle::aig_network::signal base_sig;
            
            if (reg_child.isTerminal()) {
                base_sig = aig.get_constant(true); 
            } else {
                auto it = cache.find(reg_child.GetMTBDD());
                assert(it != cache.end() && "Логическая ошибка: узел не найден в кэше!");
                base_sig = it->second;
            }
            
            return child.isComp() ? !base_sig : base_sig;
        };

        // Формирование AIG снизу вверх (Strashing делегирован mockturtle)
        for (const sylvan::Bdd& reg_node : post_order) {
            uint32_t var_idx = reg_node.TopVar();
            
            if (var_idx >= num_vars) {
                return fail<Aig>(ErrorCode::InternalError, "bdd_to_aig: индекс переменной BDD превышает n_vars()");
            }
            
            auto x_sig = pis[var_idx];
            auto hi_sig = get_child_signal(reg_node.Then());
            auto lo_sig = get_child_signal(reg_node.Else());
            
            auto ite_sig = aig.create_ite(x_sig, hi_sig, lo_sig);
            cache[reg_node.GetMTBDD()] = ite_sig;
        }

        // Подключаем корень к Primary Output
        auto root_sig = get_child_signal(f);
        aig.create_po(root_sig);

        return ok(Aig(std::move(aig)));

    } catch (const std::bad_alloc&) {
        return fail<Aig>(ErrorCode::OutOfMemory, "bdd_to_aig: исчерпана память при построении AIG");
    } catch (const std::exception& e) {
        return fail<Aig>(ErrorCode::InternalError, std::string("bdd_to_aig internal error: ") + e.what());
    }
}

} // namespace bmm


/*
#include "bdd_to_aig.hpp"

#include <tracy/Tracy.hpp>

namespace bmm {

Result<Aig> bdd_to_aig(const Bdd& bdd) {
    ZoneScoped;
    (void)bdd;
    return fail<Aig>(ErrorCode::NotImplemented, "bdd_to_aig: не реализовано");
}

}  // namespace bmm
*/