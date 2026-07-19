#include "aig_to_anf.hpp"

#include <tracy/Tracy.hpp>
#include <tbb/concurrent_hash_map.h>
#include <tbb/task_group.h>
#include <vector>
#include <array>
#include <mutex>
#include <memory>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>

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

namespace {
// НАЙДЕНО этой сессией на РЕАЛЬНЫХ данных (не синтетике): aig_to_anf не
// укладывается в разумное время (>180с) как на EPFL router.aig (n=60,
// комбинационная логика, ВСЕГО 317 гейтов), так и на AIG, построенном из
// плотного реального ANF n=100/M=10000 (persons.iis.nsk.su) — 73646 гейтов.
// В отличие от краха в anf_to_bdd (Lace dqsize — исправлено выше по коду
// сессии, чисто инженерная недостача ресурса), здесь причина СТРУКТУРНАЯ и
// НЕ чинится перенастройкой: mul_poly на AND-гейте — это полное умножение
// полиномов Жегалкина, и представление сложной булевой функции в ANF в
// общем случае может требовать экспоненциально много мономов относительно
// размера AIG (тот же класс задачи, что и структурный взрыв BDD в
// anf_to_bdd/aig_to_bdd, только с другой стороны — здесь взрывается не BDD,
// а сам целевой ANF).
//
// ВАЖНО про router.aig (317 гейтов) — доказывает, что СТАТИЧЕСКАЯ проверка
// по числу гейтов/переменных AIG (аналог kMaxTruthTableVars) здесь
// принципиально не работает: маленькая схема тоже может дать
// экспоненциальный ANF, а большая (73646 гейтов) — не обязана (могла бы
// иметь компактный ANF при удачной структуре). Единственный надёжный сигнал
// — фактическое время выполнения, не догадки по входу заранее.
//
// Также опробовано (эмпирически, НЕ помогло — оставлено ниже как
// дополнительный дешёвый слой, но не основной механизм): проверка размера
// результата ПОСЛЕ mul_poly и проверка произведения длин операндов ПЕРЕД
// mul_poly — обе не останавливали реальный датасет n=100/73646 гейтов,
// потому что ни на одном отдельном гейте нет одной "катастрофической"
// операции — цена накапливается из процентов-накладных расходов BRiAl на
// КАЖДОМ из десятков тысяч гейтов, а не взрывается на одном конкретном.
//
// Рабочий фикс — дедлайн по РЕАЛЬНОМУ времени (см. kAigToAnfTimeBudget):
// проверяется периодически в ходе обхода, не зависит от того, ГДЕ именно
// накапливается стоимость (один гейт или все понемногу) — единственный
// вариант, который эмпирически подтверждённо останавливает оба реальных
// случая, а не "почти всегда работающая" структурная эвристика. Бюджет —
// заведомо больше, чем занимают все известные успешные случаи в тестах/
// датасетах этой сессии (единицы-сотни МИЛЛИсекунд), но достаточно мал,
// чтобы не ждать минуты/часы там, где ответ реально экспоненциален.
constexpr auto kAigToAnfTimeBudget = std::chrono::seconds(10);

// Оставлено как дешёвый дополнительный слой (не основной механизм — см.
// выше): отлавливает случай одного явно катастрофического умножения без
// необходимости ждать следующей проверки дедлайна.
constexpr uint64_t kMaxAnfMonomialsDuringConstruction = 1'000'000;
constexpr uint64_t kMaxAnfMultiplyOperandProduct = 4ULL * 1'000'000;
}  // namespace

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

    // Лямбда возвращает bool: false прерывает foreach_gate ДОСРОЧНО (см.
    // mockturtle::detail::foreach_element_if в networks/detail/foreach.hpp)
    // — используется, чтобы остановиться сразу, как только либо превышен
    // временной бюджет (основной механизм, см. kAigToAnfTimeBudget выше),
    // либо (дополнительно, дёшево) промежуточный ANF на каком-то гейте уже
    // явно неправдоподобно большой.
    bool exploded = false;
    const auto deadline = std::chrono::steady_clock::now() + kAigToAnfTimeBudget;
    // Проверка дедлайна раз в 256 гейтов — достаточно часто (единственный
    // катастрофически долгий гейт всё равно ловится дополнительной
    // проверкой произведения длин операндов ниже, ДО вызова mul_poly), но
    // не добавляет заметных накладных расходов на steady_clock::now() при
    // десятках тысяч гейтов.
    uint64_t gates_processed = 0;
    net.foreach_gate([&](auto node) -> bool {
        if ((++gates_processed & 0xFFu) == 0 && std::chrono::steady_clock::now() > deadline) {
            exploded = true;
            return false;
        }

        std::array<mockturtle::aig_network::signal, 2> fanins{};
        uint32_t k = 0;
        net.foreach_fanin(node, [&](auto signal) { fanins[k++] = signal; });

        auto get_signal_poly = [&](mockturtle::aig_network::signal s) -> PolType {
            const auto& p = node_polys[net.node_to_index(net.get_node(s))];
            return net.is_complemented(s) ? add_poly(p, one_poly) : p;
        };

        PolType p0 = get_signal_poly(fanins[0]);
        PolType p1 = get_signal_poly(fanins[1]);

        const uint64_t len0 = static_cast<uint64_t>(p0.length());
        const uint64_t len1 = static_cast<uint64_t>(p1.length());
        if (len0 != 0 && len1 != 0 && len0 * len1 > kMaxAnfMultiplyOperandProduct) {
            exploded = true;
            return false;
        }

        PolType& result = node_polys[net.node_to_index(node)];
        result = mul_poly(p0, p1);

        if (static_cast<uint64_t>(result.length()) > kMaxAnfMonomialsDuringConstruction) {
            exploded = true;
            return false;
        }
        return true;
    });

    if (exploded) {
        return fail<Anf>(ErrorCode::OutOfMemory,
                          "aig_to_anf: превышен бюджет времени (" +
                              std::to_string(kAigToAnfTimeBudget.count()) +
                              "с) или размер промежуточного ANF — прервано до фактического "
                              "зависания/исчерпания памяти (структурный взрыв ANF-представления, "
                              "не чинится порядком обхода)");
    }

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

    // exploded — тот же предохранитель, что и в BRiAl-ветке выше (см.
    // kAigToAnfTimeBudget/kMaxAnfMonomialsDuringConstruction и комментарий
    // там): здесь атомарный флаг, а не ранний выход из foreach_gate, потому
    // что обход рекурсивный и параллельный (tbb::task_group) — после того
    // как флаг выставлен, дальнейшие вызовы get_anf_rec немедленно
    // возвращают пустой полином вместо продолжения работы (не останавливает
    // уже запущенные parallel TBB-задачи мгновенно, но не даёт им порождать
    // новые). Дедлайн — тот же основной механизм, что в BRiAl-ветке (см.
    // комментарий у kAigToAnfTimeBudget): статические проверки размера сами
    // по себе недостаточны (empирически не останавливали реальный датасет
    // n=100/73646 гейтов — накопление стоимости по многим мелким узлам, а
    // не один катастрофический), оставлены только как дешёвый доп. слой.
    std::atomic<bool> exploded{false};
    const auto deadline = std::chrono::steady_clock::now() + kAigToAnfTimeBudget;
    std::atomic<uint64_t> calls_made{0};

    std::function<PolType(uint32_t)> get_anf_rec = [&](uint32_t node_idx) -> PolType {
        if (exploded.load(std::memory_order_relaxed)) {
            return make_zero();
        }
        if ((calls_made.fetch_add(1, std::memory_order_relaxed) & 0xFFu) == 0 &&
            std::chrono::steady_clock::now() > deadline) {
            exploded.store(true, std::memory_order_relaxed);
            return make_zero();
        }

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

        // Проверка ПЕРЕД умножением — см. kMaxAnfMultiplyOperandProduct и
        // комментарий у BRiAl-ветки выше: сам вызов mul_poly может быть
        // катастрофически долгим ДО возврата, если операнды уже большие
        // (здесь — явный O(len0*len1) вложенный цикл в mul_poly фолбэка,
        // см. начало файла — оценка точная, не эвристическая, в отличие от
        // BRiAl-ветки).
        const uint64_t len0 = static_cast<uint64_t>(p0_val.monomials().size());
        const uint64_t len1 = static_cast<uint64_t>(p1.monomials().size());
        if (len0 != 0 && len1 != 0 && len0 * len1 > kMaxAnfMultiplyOperandProduct) {
            exploded.store(true, std::memory_order_relaxed);
            return make_zero();
        }

        PolType res = mul_poly(p0_val, p1);

        if (res.monomials().size() > kMaxAnfMonomialsDuringConstruction) {
            exploded.store(true, std::memory_order_relaxed);
        }

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

    if (exploded.load(std::memory_order_relaxed)) {
        return fail<Anf>(ErrorCode::OutOfMemory,
                          "aig_to_anf: превышен бюджет времени (" +
                              std::to_string(kAigToAnfTimeBudget.count()) +
                              "с) или размер промежуточного ANF — прервано до фактического "
                              "зависания/исчерпания памяти (структурный взрыв ANF-представления, "
                              "не чинится порядком обхода)");
    }

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
