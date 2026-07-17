#include "bdd_to_aig.hpp"
#include <tracy/Tracy.hpp>
#include <mockturtle/networks/aig.hpp>
#include <sylvan_obj.hpp>
#include <vector>
#include <unordered_map>
#include <stdexcept>

namespace bmm {

constexpr uint64_t COMP_BIT = 0x8000000000000000ULL;

// Единое состояние узла для устранения дублирования структур данных
enum class NodeState : uint8_t {
    New,       // 0: Узел еще не видели
    Expanded,  // 1: Дети добавлены в стек, ждем их обработки
    Built      // 2: Сигнал построен и готов к использованию
};

struct NodeInfo {
    NodeState state = NodeState::New;
    mockturtle::aig_network::signal sig; // Валидно только при state == Built
};

struct Frame {
    sylvan::Bdd node;
    bool is_return; // false = pre-order (раскрыть), true = post-order (построить)
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

        // ЕДИНАЯ таблица: хранит и состояние, и результат.
        std::unordered_map<uint64_t, NodeInfo> node_info;
        node_info.reserve(dag_size);

        std::vector<Frame> stack;
        stack.reserve(dag_size * 2);

        const uint64_t f_reg_raw = f_syl.GetBDD() & ~COMP_BIT;
        const bool f_is_comp = (f_syl.GetBDD() & COMP_BIT) != 0;

        // КЛАДЁМ В СТЕК ТОЛЬКО РЕГУЛЯРНЫЙ УЗЕЛ
        stack.push_back({sylvan::Bdd(f_reg_raw), false});

        // Однопроходный итеративный DFS
        while (!stack.empty()) {
            Frame frame = std::move(stack.back());
            stack.pop_back();

            const uint64_t raw = frame.node.GetBDD();
            const uint64_t reg_raw = raw & ~COMP_BIT;

            // Получаем или создаем запись в единой таблице (1 хэш-поиск)
            auto& info = node_info[reg_raw];

            if (frame.is_return) {
                // ЭТАП 2: Построение (post-order)
                if (info.state == NodeState::Built) {
                    continue;
                }

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
                        auto it = node_info.find(c_reg);
                        if (it == node_info.end() || it->second.state != NodeState::Built) {
                            throw std::logic_error("bdd_to_aig: child node not built (invariant violated)");
                        }
                        s = it->second.sig;
                    }
                    return c_comp ? !s : s;
                };

                const auto hi_sig = get_sig(frame.node.Then());
                const auto lo_sig = get_sig(frame.node.Else());

                info.sig = aig.create_ite(x_sig, hi_sig, lo_sig);
                info.state = NodeState::Built;

            } else {
                // ЭТАП 1: Первый визит (pre-order)
                if (info.state == NodeState::Built) {
                    continue;
                }
                if (info.state == NodeState::Expanded) {
                    continue;
                }

                // Помечаем как Expanded
                info.state = NodeState::Expanded;

                // 1. Сначала кладем задачу на построение этого узла (post-order)
                stack.push_back({frame.node, true});

                // 2. Затем кладем детей (pre-order, LIFO)
                const sylvan::Bdd T = frame.node.Then();
                const sylvan::Bdd E = frame.node.Else();

                const uint64_t T_reg = T.GetBDD() & ~COMP_BIT;
                const uint64_t E_reg = E.GetBDD() & ~COMP_BIT;

                // Вспомогательная лямбда: кладем в стек ТОЛЬКО регулярные узлы!
                auto push_if_needed = [&](uint64_t child_reg) {
                    auto it = node_info.find(child_reg);
                    if (it == node_info.end() || (it->second.state != NodeState::Built && it->second.state != NodeState::Expanded)) {
                        stack.push_back({sylvan::Bdd(child_reg), false});
                    }
                };

                if (!T.isZero() && !T.isOne()) {
                    push_if_needed(T_reg);
                }
                if (!E.isZero() && !E.isOne()) {
                    push_if_needed(E_reg);
                }
            }
        }

        // Извлекаем результат корня
        auto it = node_info.find(f_reg_raw);
        if (it == node_info.end() || it->second.state != NodeState::Built) {
            return fail<Aig>(ErrorCode::InvalidArgument, "bdd_to_aig: внутренняя ошибка, корень не построен");
        }
        
        mockturtle::aig_network::signal root_sig = it->second.sig;
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