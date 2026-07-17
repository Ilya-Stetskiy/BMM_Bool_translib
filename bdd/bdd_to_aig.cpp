#include "bdd_to_aig.hpp"
#include <tracy/Tracy.hpp>
#include <mockturtle/networks/aig.hpp>
#include <sylvan_obj.hpp>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

namespace bmm {

constexpr uint64_t COMP_BIT = 0x8000000000000000ULL;

// Кадр стека для имитации рекурсивного вызова
struct Frame {
    sylvan::Bdd node;
    bool expanded; // false = первый визит, true = дети обработаны, пора строить
};

Result<Aig> bdd_to_aig(const Bdd& f) {
    ZoneScoped;

    try {
        mockturtle::aig_network aig;
        const uint32_t num_vars = f.n_vars();

        // Создаём Primary Inputs заранее
        std::vector<mockturtle::aig_network::signal> pis(num_vars);
        for (uint32_t i = 0; i < num_vars; ++i) {
            pis[i] = aig.create_pi();
        }

        const sylvan::Bdd f_syl = f.raw();

        // Быстрый путь для констант
        if (f_syl.isZero()) {
            aig.create_po(aig.get_constant(false));
            return ok(Aig(std::move(aig)));
        }
        if (f_syl.isOne()) {
            aig.create_po(aig.get_constant(true));
            return ok(Aig(std::move(aig)));
        }

        const size_t dag_size = std::max<size_t>(f_syl.NodeCount(), 64);

        // Кэш: regular_id -> signal (также служит маркером состояния "Built")
        std::unordered_map<uint64_t, mockturtle::aig_network::signal> cache;
        cache.reserve(dag_size);

        // Expanded: regular_id (маркер состояния "Gray" / "в стеке")
        std::unordered_set<uint64_t> expanded;
        expanded.reserve(dag_size);

        std::vector<Frame> stack;
        stack.reserve(dag_size * 2);

        const uint64_t f_reg_raw = f_syl.GetBDD() & ~COMP_BIT;
        const bool f_is_comp = (f_syl.GetBDD() & COMP_BIT) != 0;

        stack.push_back({sylvan::Bdd(f_reg_raw), false});

        // Однопроходный итеративный DFS
        while (!stack.empty()) {
            Frame frame = std::move(stack.back());
            stack.pop_back();

            const uint64_t raw = frame.node.GetBDD();
            const uint64_t reg_raw = raw & ~COMP_BIT;
            const bool is_comp = (raw & COMP_BIT) != 0;

            if (frame.expanded) {
                // ЭТАП 2: Дети гарантированно уже обработаны и лежат в cache.
                const uint32_t var_idx = frame.node.TopVar();
                if (var_idx >= num_vars) {
                    return fail<Aig>(ErrorCode::InvalidArgument, "bdd_to_aig: индекс BDD превышает n_vars()");
                }

                const auto x_sig = pis[var_idx];

                // Лямбда для чистого получения сигнала ребёнка
                auto get_sig = [&](const sylvan::Bdd& child) {
                    const uint64_t c_raw = child.GetBDD();
                    const bool c_comp = (c_raw & COMP_BIT) != 0;
                    const uint64_t c_reg = c_raw & ~COMP_BIT;

                    mockturtle::aig_network::signal s;
                    const sylvan::Bdd c_reg_node(c_reg);
                    
                    if (c_reg_node.isZero()) {
                        s = aig.get_constant(false);
                    } else if (c_reg_node.isOne()) {
                        s = aig.get_constant(true);
                    } else {
                        s = cache.at(c_reg); // Гарантированно есть, так как мы в post-order
                    }
                    return c_comp ? !s : s;
                };

                const auto hi_sig = get_sig(frame.node.Then());
                const auto lo_sig = get_sig(frame.node.Else());

                const auto ite_sig = aig.create_ite(x_sig, hi_sig, lo_sig);
                
                // Помечаем как Built, добавляя в кэш
                cache.try_emplace(reg_raw, ite_sig);

            } else {
                // ЭТАП 1: Первый визит в узел
                
                // Если уже Built (есть в кэше) или уже в стеке (Expanded), пропускаем
                if (cache.count(reg_raw) || expanded.count(reg_raw)) {
                    continue;
                }

                // Помечаем как Expanded (Gray)
                expanded.insert(reg_raw);

                // Сначала кладем в стек задачу на построение этого узла (после детей)
                stack.push_back({frame.node, true});

                // Затем кладем детей (они будут обработаны первыми, LIFO)
                const sylvan::Bdd T = frame.node.Then();
                const sylvan::Bdd E = frame.node.Else();

                const uint64_t T_reg = T.GetBDD() & ~COMP_BIT;
                const uint64_t E_reg = E.GetBDD() & ~COMP_BIT;

                // Кладем в стек только нетерминальные узлы, которые еще не Built и не Expanded
                if (!T.isZero() && !T.isOne() && !cache.count(T_reg) && !expanded.count(T_reg)) {
                    stack.push_back({sylvan::Bdd(T_reg), false});
                }
                if (!E.isZero() && !E.isOne() && !cache.count(E_reg) && !expanded.count(E_reg)) {
                    stack.push_back({sylvan::Bdd(E_reg), false});
                }
            }
        }

        // Извлекаем сигнал корня
        auto root_it = cache.find(f_reg_raw);
        if (root_it == cache.end()) {
            return fail<Aig>(ErrorCode::InvalidArgument, "bdd_to_aig: внутренняя ошибка, корень не найден");
        }
        
        mockturtle::aig_network::signal root_sig = root_it->second;
        if (f_is_comp) root_sig = !root_sig;

        aig.create_po(root_sig);
        return ok(Aig(std::move(aig)));

    } catch (const std::bad_alloc&) {
        return fail<Aig>(ErrorCode::OutOfMemory, "bdd_to_aig: исчерпана память");
    } catch (const std::exception& e) {
        return fail<Aig>(ErrorCode::InvalidArgument, std::string("bdd_to_aig error: ") + e.what());
    }
}

} // namespace bmm