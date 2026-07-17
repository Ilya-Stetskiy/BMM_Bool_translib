#include "bdd_to_aig.hpp"
#include <tracy/Tracy.hpp>
#include <mockturtle/networks/aig.hpp>
#include <sylvan_obj.hpp>
#include <vector>
#include <unordered_map>
#include <stdexcept>

namespace bmm {

constexpr uint64_t COMP_BIT = 0x8000000000000000ULL;

// Выделенная inline-функция для работы с комплементными рёбрами
inline mockturtle::aig_network::signal
lookup_signal(
    const sylvan::Bdd& node,
    mockturtle::aig_network& aig,
    const std::unordered_map<uint64_t, mockturtle::aig_network::signal>& cache)
{
    const uint64_t raw = node.GetBDD();
    const bool comp = (raw & COMP_BIT) != 0;
    const sylvan::Bdd reg(raw & ~COMP_BIT);

    mockturtle::aig_network::signal s =
        reg.isZero()
            ? aig.get_constant(false)
            : reg.isOne()
                ? aig.get_constant(true)
                : cache.at(reg.GetBDD());

    return comp ? !s : s;
}

// Кадр стека для итеративного DFS
struct Frame {
    sylvan::Bdd node;
    uint8_t stage;  // 0 = вход, 1 = выход (оба ребёнка готовы)
};

Result<Aig> bdd_to_aig(const Bdd& f) {
    ZoneScoped;

    try {
        mockturtle::aig_network aig;
        const uint32_t num_vars = f.n_vars();

        // Создаём Primary Inputs заранее (для единообразия интерфейса)
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

        const uint64_t f_raw = f_syl.GetBDD();
        const uint64_t f_reg_raw = f_raw & ~COMP_BIT;
        const bool f_is_comp = (f_raw & COMP_BIT) != 0;

        // Оценка размера для предвыделения памяти
        const size_t dag_size = std::max<size_t>(f_syl.NodeCount(), 64);

        // Кэш с предвыделением памяти
        std::unordered_map<uint64_t, mockturtle::aig_network::signal> cache;
        cache.reserve(dag_size);
        
        // Стек кадров для итеративного DFS
        std::vector<Frame> stack;
        stack.reserve(dag_size * 2);

        stack.push_back({sylvan::Bdd(f_reg_raw), 0});

        // Итеративный DFS с кадрами
        while (!stack.empty()) {
            auto [curr, stage] = stack.back();
            stack.pop_back();

            const uint64_t curr_raw = curr.GetBDD();
            const uint64_t curr_reg_raw = curr_raw & ~COMP_BIT;

            // Stage 1: оба ребёнка готовы, строим ITE
            if (stage == 1) {
                const uint32_t var_idx = curr.TopVar();
                if (var_idx >= num_vars) {
                    return fail<Aig>(ErrorCode::InvalidArgument, "bdd_to_aig: индекс BDD превышает n_vars()");
                }
                
                const auto x_sig = pis[var_idx];
                const auto hi_sig = lookup_signal(curr.Then(), aig, cache);
                const auto lo_sig = lookup_signal(curr.Else(), aig, cache);
                
                const auto ite_sig = aig.create_ite(x_sig, hi_sig, lo_sig);
                
                cache.try_emplace(curr_reg_raw, ite_sig);
                continue;
            }

            // Stage 0: вход в узел
            // Проверяем, уже ли обработан
            if (cache.count(curr_reg_raw)) {
                continue;
            }

            const sylvan::Bdd T = curr.Then();
            const sylvan::Bdd E = curr.Else();
            
            const uint64_t T_reg_raw = T.GetBDD() & ~COMP_BIT;
            const uint64_t E_reg_raw = E.GetBDD() & ~COMP_BIT;

            // Проверяем готовность детей
            const bool T_ready = T.isZero() || T.isOne() || cache.count(T_reg_raw);
            const bool E_ready = E.isZero() || E.isOne() || cache.count(E_reg_raw);

            if (T_ready && E_ready) {
                // Оба ребёнка готовы, сразу переходим к stage 1
                stack.push_back({curr, 1});
            } else {
                // Пушим текущий узел с stage 1 (выйдет после детей)
                stack.push_back({curr, 1});
                
                // Пушим непосещённых детей с stage 0
                if (!T_ready) {
                    stack.push_back({sylvan::Bdd(T_reg_raw), 0});
                }
                if (!E_ready) {
                    stack.push_back({sylvan::Bdd(E_reg_raw), 0});
                }
            }
        }

        // Подключаем корень с учётом комплемент-бита
        mockturtle::aig_network::signal root_sig = cache.at(f_reg_raw);
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