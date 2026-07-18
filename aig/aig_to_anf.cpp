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
// P*P = P — не частный случай, а тождество для ЛЮБОГО элемента булева
// кольца GF(2)[x]/(x_i^2-x_i): в кольце характеристики 2 (P+Q)^2 = P^2+Q^2,
// и по индукции от одночленов (x_i^2=x_i по определению кольца) любой
// многочлен идемпотентен относительно умножения. isZero()/isOne() отсекают
// ещё две тривиальные формы без обращения к общему (дорогому, полному
// ZDD-произведению) BoolePolynomial::operator*. Раньше эти проверки
// отсутствовали — каждый AND-узел платил полное умножение, даже когда оба
// операнда совпадали или один был константой.
inline PolType mul_poly(const PolType& p1, const PolType& p2) {
    if (p1.isZero() || p2.isZero()) return BoolePolynomial(p1.ring());
    if (p1.isOne()) return p2;
    if (p2.isOne()) return p1;
    if (p1 == p2) return p1;
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
inline bool is_zero(const PolType& p) { return p.monomials().empty(); }
inline bool is_one(const PolType& p) {
    return p.monomials().size() == 1 && p.monomials().begin()->empty();
}
// Тот же P*P=P и isZero/isOne, что и в BRiAl-ветке выше — тождество кольца
// не зависит от представления полинома, только от алгебры.
inline PolType mul_poly(const PolType& p1, const PolType& p2) {
    if (is_zero(p1) || is_zero(p2)) return AnfFallback();
    if (is_one(p1)) return p2;
    if (is_one(p2)) return p1;
    if (p1.monomials() == p2.monomials()) return p1;
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

  try {
#if BMM_HAVE_BRIAL
    // Топологический проход БЕЗ рекурсии и БЕЗ TBB. Это сознательный отказ
    // от параллелизма, а не недосмотр: BRiAl (CUDD-based ZDD-менеджер) не
    // потокобезопасен — подтверждено ThreadSanitizer (гонка в
    // CCuddCore::release(), см. aig/README.md §1.3), поэтому конкурентный
    // обход в этой ветке не дал бы параллелизма, только риск гонки на общем
    // BoolePolyRing. net.foreach_gate обходит узлы в порядке, где фанины
    // уже вычислены (инвариант mockturtle::aig_network: индекс фанина
    // всегда меньше индекса узла), поэтому node_polys[child] всегда готов к
    // моменту обработки node_polys[node] — мемоизация здесь просто индекс
    // в массиве, отдельная хэш-таблица/мьютексы на узел не нужны.
    auto& ring = get_ring(n);

    std::vector<PolType> node_polys(net.size(), make_zero(ring));

    uint32_t const_node_idx = net.node_to_index(net.get_node(net.get_constant(false)));
    node_polys[const_node_idx] = make_zero(ring);

    uint32_t pi_counter = 0;
    net.foreach_pi([&](auto node) {
        node_polys[net.node_to_index(node)] = make_var(ring, pi_counter++);
    });

    const PolType one_poly = make_one(ring);

    net.foreach_gate([&](auto node) {
        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        net.foreach_fanin(node, [&](auto signal) { fanins[k++] = signal; });

        auto get_signal_poly = [&](mockturtle::aig_network::signal s) -> PolType {
            const auto& p = node_polys[net.node_to_index(net.get_node(s))];
            return net.is_complemented(s) ? add_poly(p, one_poly) : p;
        };

        PolType p0 = get_signal_poly(fanins[0]);
        PolType p1 = get_signal_poly(fanins[1]);
        node_polys[net.node_to_index(node)] = mul_poly(p0, p1);
    });

    mockturtle::aig_network::signal po_sig;
    net.foreach_po([&](auto signal) { po_sig = signal; });

    PolType final_poly = node_polys[net.node_to_index(net.get_node(po_sig))];
    if (net.is_complemented(po_sig)) {
        final_poly = add_poly(final_poly, one_poly);
    }

    return ok<Anf>(Anf(std::move(final_poly), n));

#else
    // AnfFallback: BRiAl недоступен в этой сборке. Здесь, в отличие от
    // BRiAl-ветки выше, конкурентный обход безопасен — AnfFallback работает
    // с обычным std::set без разделяемого глобального менеджера, поэтому
    // TBB действительно даёт параллелизм (см. aig/README.md §1.3). Эта
    // ветка не тронута относительно предыдущей версии: рекурсия +
    // мемоизация через shared_ptr<MemoEntry>, mutex защищает только
    // мгновенную публикацию результата (не удерживается поперёк
    // tg.wait() — тот же паттерн, что и фикс дедлока в aig/tt_to_aig.cpp).
    struct MemoEntry {
        std::atomic<bool> ready{false};
        std::mutex mtx;
        std::optional<PolType> val;
    };
    using MemoMap = tbb::concurrent_hash_map<uint32_t, std::shared_ptr<MemoEntry>>;
    MemoMap memo;

    std::vector<uint32_t> pi_indices(net.size(), 0);
    uint32_t pi_counter = 0;
    net.foreach_pi([&](auto node) {
        pi_indices[net.node_to_index(node)] = pi_counter++;
    });

    uint32_t const_node_idx = net.node_to_index(net.get_node(net.get_constant(false)));

    std::vector<mockturtle::aig_network::node> index_to_node(net.size());
    net.foreach_node([&](auto node) {
        index_to_node[net.node_to_index(node)] = node;
    });

    std::function<PolType(uint32_t)> get_anf_rec = [&](uint32_t node_idx) -> PolType {
        if (node_idx == const_node_idx) {
            return make_zero();
        }

        auto node = index_to_node[node_idx];
        if (net.is_pi(node)) {
            return make_var(pi_indices[node_idx]);
        }

        MemoMap::accessor acc;
        if (memo.insert(acc, node_idx)) {
            acc->second = std::make_shared<MemoEntry>();
        }
        auto entry = acc->second;
        acc.release();

        if (entry->ready) {
            return *(entry->val);
        }

        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        net.foreach_fanin(node, [&](auto signal) { fanins[k++] = signal; });

        uint32_t child0 = net.node_to_index(net.get_node(fanins[0]));
        uint32_t child1 = net.node_to_index(net.get_node(fanins[1]));

        auto apply_comp = [&](PolType p, bool comp) -> PolType {
            return comp ? add_poly(p, make_one()) : p;
        };

        std::optional<PolType> p0;
        tbb::task_group tg;
        tg.run([&] { p0 = get_anf_rec(child0); });
        PolType p1 = get_anf_rec(child1);
        tg.wait();

        PolType p0_val = apply_comp(*p0, net.is_complemented(fanins[0]));
        p1 = apply_comp(p1, net.is_complemented(fanins[1]));

        PolType res = mul_poly(p0_val, p1);

        {
            std::lock_guard<std::mutex> lock(entry->mtx);
            if (!entry->ready) {
                entry->val = res;
                entry->ready = true;
            }
        }
        return res;
    };

    mockturtle::aig_network::signal po_sig;
    net.foreach_po([&](auto signal) { po_sig = signal; });

    uint32_t po_node_idx = net.node_to_index(net.get_node(po_sig));
    PolType final_poly = get_anf_rec(po_node_idx);

    if (net.is_complemented(po_sig)) {
        final_poly = add_poly(final_poly, make_one());
    }

    return ok<Anf>(Anf(std::move(final_poly), n));
#endif
  } catch (const std::bad_alloc&) {
      // ДОБАВЛЕНО: раньше не было ни одного catch в этой функции — рост
      // BoolePolynomial/AnfFallback не ограничен заранее по построению
      // (в отличие от TruthTable-выходов), bad_alloc распространялся бы как
      // необработанное исключение через границу Result<T>.
      return fail<Anf>(ErrorCode::OutOfMemory, "aig_to_anf: исчерпана память");
  }
}

}  // namespace bmm
