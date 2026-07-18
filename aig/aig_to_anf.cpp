#include "aig_to_anf.hpp"

#include <tracy/Tracy.hpp>
#include <tbb/concurrent_hash_map.h>
#include <tbb/task_group.h>
#include <vector>
#include <array>
#include <mutex>
#include <memory>
#include <atomic>
#include <functional>
#include <optional>

namespace bmm {

#if BMM_HAVE_BRIAL
using PolType = BoolePolynomial;

inline PolType make_zero(const BoolePolyRing& ring) {
    return BoolePolynomial(ring);
}
inline PolType make_one(const BoolePolyRing& ring) {
    return BoolePolynomial(BooleMonomial(ring));
}
inline PolType make_var(const BoolePolyRing& ring, uint32_t var_idx) {
    return BoolePolynomial(ring.variable(var_idx));
}
inline PolType add_poly(const PolType& p1, const PolType& p2) {
    return p1 + p2;
}
inline PolType mul_poly(const PolType& p1, const PolType& p2) {
    return p1 * p2;
}
#else
using PolType = AnfFallback;

namespace {
inline PolType make_zero() {
    return AnfFallback();
}
inline PolType make_one() {
    AnfFallback res;
    res.add_monomial({});
    return res;
}
inline PolType make_var(uint32_t var_idx) {
    AnfFallback res;
    res.add_monomial({var_idx});
    return res;
}
inline PolType add_poly(const PolType& p1, const PolType& p2) {
    AnfFallback res = p1;
    for (const auto& m : p2.monomials()) {
        res.add_monomial(m);
    }
    return res;
}
inline PolType mul_poly(const PolType& p1, const PolType& p2) {
    AnfFallback res;
    for (const auto& m1 : p1.monomials()) {
        for (const auto& m2 : p2.monomials()) {
            Monomial product_mono;
            product_mono.reserve(m1.size() + m2.size());
            std::set_union(m1.begin(), m1.end(), m2.begin(), m2.end(), std::back_inserter(product_mono));
            res.add_monomial(std::move(product_mono));
        }
    }
    return res;
}
} // namespace
#endif
#if BMM_HAVE_BRIAL
namespace {
BoolePolyRing& get_ring(uint32_t n) {
    static std::unique_ptr<BoolePolyRing> active_ring;
    static uint32_t active_n = 0xffffffff;
    static std::mutex ring_mutex;
    std::lock_guard<std::mutex> lock(ring_mutex);
    if (!active_ring || active_n != n) {
        active_ring = std::make_unique<BoolePolyRing>(n == 0 ? 1 : n);
        active_n = n;
    }
    return *active_ring;
}
} // namespace
#endif

Result<Anf> aig_to_anf(const Aig& aig) {
    ZoneScoped;
    const auto& net = aig.raw();
    if (net.num_pos() != 1) {
        return fail<Anf>(ErrorCode::Unsupported, "aig_to_anf: ожидается ровно один PO");
    }

    const uint32_t n = aig.n_vars();

#if BMM_HAVE_BRIAL
    auto& ring = get_ring(n);
#endif

    struct MemoEntry {
        std::atomic<bool> ready{false};
        std::mutex mtx;
        std::optional<PolType> val;
    };
    using MemoMap = tbb::concurrent_hash_map<uint32_t, std::shared_ptr<MemoEntry>>;
    MemoMap memo;

    // PI index mapping
    std::vector<uint32_t> pi_indices(net.size(), 0);
    uint32_t pi_counter = 0;
    net.foreach_pi([&](auto node) {
        pi_indices[net.node_to_index(node)] = pi_counter++;
    });

    // Helper for constant node
    uint32_t const_node_idx = net.node_to_index(net.get_node(net.get_constant(false)));

    // Index to node mapping to avoid un-thread-safe index_to_node lookups
    std::vector<mockturtle::aig_network::node> index_to_node(net.size());
    net.foreach_node([&](auto node) {
        index_to_node[net.node_to_index(node)] = node;
    });

    std::function<PolType(uint32_t)> get_anf_rec = [&](uint32_t node_idx) -> PolType {
        // 1. Const check
        if (node_idx == const_node_idx) {
#if BMM_HAVE_BRIAL
            return make_zero(ring);
#else
            return make_zero();
#endif
        }

        // 2. PI check
        auto node = index_to_node[node_idx];
        if (net.is_pi(node)) {
            uint32_t var_idx = pi_indices[node_idx];
#if BMM_HAVE_BRIAL
            return make_var(ring, var_idx);
#else
            return make_var(var_idx);
#endif
        }

        // 3. Memoization
        MemoMap::accessor acc;
        if (memo.insert(acc, node_idx)) {
            acc->second = std::make_shared<MemoEntry>();
        }
        auto entry = acc->second;
        acc.release();

        if (entry->ready) {
            return *(entry->val);
        }

        // ИСПРАВЛЕНО (тот же баг и то же исправление, что в
        // aig/tt_to_aig.cpp::build_aig_rec — см. подробный разбор там и в
        // aig/README.md §1.1): раньше здесь стоял std::lock_guard<std::mutex>
        // lock(entry->mtx), удерживаемый поперёк tg.run()/tg.wait() ниже
        // (ветка AnfFallback) — поток, ждущий чужой entry->mtx, становится
        // недоступен планировщику TBB как воркер; при коллизиях на один
        // memo-ключ это гарантированно исчерпывает арену (подтверждено
        // стек-трейсом реального зависания в tt_to_aig.cpp). Здесь баг не
        // проявлялся эмпирически только потому, что в текущей сборке
        // BMM_HAVE_BRIAL=1 и эта ветка (AnfFallback) не выполняется — но
        // код опасен структурно точно так же, как был tt_to_aig.cpp до
        // фикса. Не блокируем поток ожиданием чужой работы — конкурентные
        // вызовы для одного узла считают его независимо и избыточно.

        // Compute it (it's a gate)
        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        net.foreach_fanin(node, [&](auto signal) { fanins[k++] = signal; });

        uint32_t child0 = net.node_to_index(net.get_node(fanins[0]));
        uint32_t child1 = net.node_to_index(net.get_node(fanins[1]));

        auto apply_comp = [&](PolType p, bool comp) -> PolType {
            if (comp) {
#if BMM_HAVE_BRIAL
                return add_poly(p, make_one(ring));
#else
                return add_poly(p, make_one());
#endif
            }
            return p;
        };

#if BMM_HAVE_BRIAL
        PolType p0_val = apply_comp(get_anf_rec(child0), net.is_complemented(fanins[0]));
        PolType p1 = apply_comp(get_anf_rec(child1), net.is_complemented(fanins[1]));
#else
        std::optional<PolType> p0;
        tbb::task_group tg;
        tg.run([&] { p0 = get_anf_rec(child0); });
        PolType p1 = get_anf_rec(child1);
        tg.wait();

        PolType p0_val = apply_comp(*p0, net.is_complemented(fanins[0]));
        p1 = apply_comp(p1, net.is_complemented(fanins[1]));
#endif

        // Multiply
        PolType res = mul_poly(p0_val, p1);

        {
            // Короткая критическая секция БЕЗ рекурсии внутри — только
            // публикация уже готового res (см. комментарий выше и
            // aig/tt_to_aig.cpp за полным обоснованием). Если нас
            // опередили — оставляем чужое значение каноническим в entry,
            // но возвращаем СЕБЕ наш собственный res (логически
            // эквивалентен).
            std::lock_guard<std::mutex> lock(entry->mtx);
            if (!entry->ready) {
                entry->val = res;
                entry->ready = true;
            }
        }
        return res;
    };

    // Evaluate PO
    mockturtle::aig_network::signal po_sig;
    net.foreach_po([&](auto signal) { po_sig = signal; });

    uint32_t po_node_idx = net.node_to_index(net.get_node(po_sig));
    PolType final_poly = get_anf_rec(po_node_idx);

    if (net.is_complemented(po_sig)) {
#if BMM_HAVE_BRIAL
        final_poly = add_poly(final_poly, make_one(ring));
#else
        final_poly = add_poly(final_poly, make_one());
#endif
    }

    return ok<Anf>(Anf(std::move(final_poly), n));
}

}  // namespace bmm
