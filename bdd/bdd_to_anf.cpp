#include "bdd_to_anf.hpp"
#include <tracy/Tracy.hpp>
#include <sylvan_obj.hpp>
#include <kitty/operations.hpp>
#include <parallel_hashmap/phmap.h>
#include <vector>
#include <stdexcept>
#include <cassert>

#if BMM_HAVE_BRIAL
#include <polybori.h>
#include <polybori/routines/pbori_routines_misc.h>
#endif

namespace bmm {

namespace detail {

constexpr uint64_t COMP_BIT = 0x8000000000000000ULL;

// ============================================================================
// Этап 1: Плотное представление BDD
// ============================================================================

struct DenseNode {
    uint32_t var;   // логический индекс переменной (уже пропущен через var_at_level)
    uint32_t lo;
    uint32_t hi;
    bool lo_comp;
    bool hi_comp;
};

struct DenseBDD {
    std::vector<DenseNode> nodes;
    uint32_t root;
    bool root_comp;

    static constexpr uint32_t TERMINAL_FALSE = 0;
    static constexpr uint32_t TERMINAL_TRUE = 1;
};

// ============================================================================
// Этап 2: Построение DenseBDD из Sylvan (строго Option B: работа через regular)
// ============================================================================
//
// Портировано с origin/egor: физический уровень Sylvan-узла (TopVar())
// не обязан совпадать с индексом переменной — единственный сейчас
// производитель Bdd с нестандартным порядком уровней — anf_to_bdd (см.
// core/common.hpp::Bdd::var_at_level, anf/anf_to_bdd.cpp "Часть 2"). На
// ветке egor этого механизма ещё не существовало, поэтому DenseBuilder
// принимает исходный bmm::Bdd (а не голый sylvan::Bdd) и переводит
// TopVar() в логический индекс через var_at_level() при заполнении
// DenseNode::var — без этого функция тихо строила бы ANF по ФИЗИЧЕСКОМУ
// уровню вместо переменной для любого входа от anf_to_bdd() с нетривиальной
// перестановкой (умолчание — VariableOrderHeuristic::Force).
class DenseBuilder {
public:
    static DenseBDD build(const Bdd& bdd) {
        DenseBuilder builder(bdd);
        builder.traverse(bdd.raw());

        DenseBDD result;
        result.nodes = std::move(builder.nodes);

        uint64_t root_raw = bdd.raw().GetBDD();
        bool root_comp = (root_raw & COMP_BIT) != 0;
        uint64_t root_reg = root_raw & ~COMP_BIT;

        auto it = builder.raw_to_id.find(root_reg);
        if (it == builder.raw_to_id.end()) {
            throw std::logic_error("DenseBuilder: root node not found");
        }

        result.root = it->second;
        result.root_comp = root_comp;

        return result;
    }

private:
    explicit DenseBuilder(const Bdd& bdd) : bdd_(bdd) {}

    const Bdd& bdd_;
    phmap::flat_hash_map<uint64_t, uint32_t> raw_to_id;
    std::vector<DenseNode> nodes;

    void traverse(const sylvan::Bdd& bdd) {
        struct Frame {
            uint64_t raw;
            bool is_return;
        };

        std::vector<Frame> stack;
        phmap::flat_hash_set<uint64_t> visited;

        uint64_t root_raw = bdd.GetBDD();
        uint64_t root_reg = root_raw & ~COMP_BIT;

        stack.push_back({root_reg, false});

        const size_t estimated_nodes = bdd.NodeCount() + 2;
        nodes.reserve(estimated_nodes);
        raw_to_id.reserve(estimated_nodes);
        visited.reserve(estimated_nodes);

        nodes.push_back({0, 0, 0, false, false});
        nodes.push_back({0, 0, 0, false, false});

        while (!stack.empty()) {
            Frame frame = stack.back();
            stack.pop_back();

            uint64_t reg_raw = frame.raw;

            if (frame.is_return) {
                if (raw_to_id.find(reg_raw) != raw_to_id.end()) {
                    continue;
                }

                sylvan::Bdd node(reg_raw);
                if (node.isTerminal()) {
                    continue;
                }

                uint32_t var = bdd_.var_at_level(node.TopVar());

                sylvan::Bdd then_bdd = node.Then();
                sylvan::Bdd else_bdd = node.Else();

                uint64_t then_raw = then_bdd.GetBDD();
                uint64_t else_raw = else_bdd.GetBDD();

                bool then_comp = (then_raw & COMP_BIT) != 0;
                bool else_comp = (else_raw & COMP_BIT) != 0;

                uint64_t then_reg = then_raw & ~COMP_BIT;
                uint64_t else_reg = else_raw & ~COMP_BIT;

                // КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: создаем regular-обертки для проверок
                sylvan::Bdd then_regular(then_reg);
                sylvan::Bdd else_regular(else_reg);

                uint32_t high_id, low_id;

                // ПРОВЕРЯЕМ ТОЛЬКО regular узлы, чтобы избежать двойного учета complement
                if (then_regular.isZero()) {
                    high_id = DenseBDD::TERMINAL_FALSE;
                } else if (then_regular.isOne()) {
                    high_id = DenseBDD::TERMINAL_TRUE;
                } else {
                    auto it = raw_to_id.find(then_reg);
                    if (it == raw_to_id.end()) {
                        throw std::logic_error("DenseBuilder: then child not found");
                    }
                    high_id = it->second;
                }

                if (else_regular.isZero()) {
                    low_id = DenseBDD::TERMINAL_FALSE;
                } else if (else_regular.isOne()) {
                    low_id = DenseBDD::TERMINAL_TRUE;
                } else {
                    auto it = raw_to_id.find(else_reg);
                    if (it == raw_to_id.end()) {
                        throw std::logic_error("DenseBuilder: else child not found");
                    }
                    low_id = it->second;
                }

                uint32_t new_id = nodes.size();
                nodes.push_back({var, low_id, high_id, else_comp, then_comp});
                raw_to_id[reg_raw] = new_id;

            } else {
                if (visited.find(reg_raw) != visited.end()) {
                    continue;
                }

                sylvan::Bdd node(reg_raw);
                if (node.isTerminal()) {
                    continue;
                }

                visited.insert(reg_raw);
                stack.push_back({reg_raw, true});

                sylvan::Bdd then_bdd = node.Then();
                sylvan::Bdd else_bdd = node.Else();

                uint64_t then_reg = then_bdd.GetBDD() & ~COMP_BIT;
                uint64_t else_reg = else_bdd.GetBDD() & ~COMP_BIT;

                // КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: проверяем терминальность regular узлов
                sylvan::Bdd then_regular(then_reg);
                sylvan::Bdd else_regular(else_reg);

                if (!then_regular.isTerminal()) {
                    stack.push_back({then_reg, false});
                }
                if (!else_regular.isTerminal()) {
                    stack.push_back({else_reg, false});
                }
            }
        }
    }
};

// ============================================================================
// Этап 3: Вычисление ANF через алгоритм Давио
// ============================================================================

enum class ComputeStatus {
    SUCCESS,
    FALLBACK_NEEDED
};

struct ComputeResult {
    ComputeStatus status;
#if BMM_HAVE_BRIAL
    BoolePolynomial poly;
#else
    AnfFallback fallback;
#endif
};

class AnfEngine {
public:
    static ComputeResult compute(const DenseBDD& dense, uint32_t n_vars, size_t threshold) {
#if BMM_HAVE_BRIAL
        BoolePolyRing ring(n_vars);

        std::vector<BoolePolynomial> node_anf;
        node_anf.reserve(dense.nodes.size());
        for (size_t i = 0; i < dense.nodes.size(); ++i) {
            node_anf.emplace_back(ring);
        }

        node_anf[DenseBDD::TERMINAL_FALSE] = BoolePolynomial(ring);
        node_anf[DenseBDD::TERMINAL_TRUE] = ring.one();

        for (size_t i = 2; i < dense.nodes.size(); ++i) {
            const DenseNode& node = dense.nodes[i];

            if (node.lo > 1) assert(node.lo < i);
            if (node.hi > 1) assert(node.hi < i);

            uint32_t var = node.var;
            uint32_t lo_id = node.lo;
            uint32_t hi_id = node.hi;
            bool lo_comp = node.lo_comp;
            bool hi_comp = node.hi_comp;

            const BoolePolynomial& low_poly_ref = node_anf[lo_id];
            const BoolePolynomial& high_poly_ref = node_anf[hi_id];

            // Complement применяется ровно один раз, к результату regular узла
            BoolePolynomial low_poly = lo_comp ? (ring.one() + low_poly_ref) : low_poly_ref;
            BoolePolynomial high_poly = hi_comp ? (ring.one() + high_poly_ref) : high_poly_ref;

            BoolePolynomial diff = low_poly + high_poly;

            if (diff.length() > threshold) {
                return {ComputeStatus::FALLBACK_NEEDED, BoolePolynomial(ring)};
            }

            BoolePolynomial x_term = ring.variable(var);
            BoolePolynomial node_poly = low_poly + (x_term * diff);

            if (node_poly.length() > threshold) {
                return {ComputeStatus::FALLBACK_NEEDED, BoolePolynomial(ring)};
            }

            node_anf[i] = std::move(node_poly);
        }

        BoolePolynomial result = node_anf[dense.root];
        if (dense.root_comp) {
            result = ring.one() + result;
        }

        return {ComputeStatus::SUCCESS, std::move(result)};
#else
        return {ComputeStatus::FALLBACK_NEEDED, AnfFallback()};
#endif
    }
};

// ============================================================================
// Этап 4: Fallback через TT + Möbius Transform
// ============================================================================
//
// Идёт через Bdd::to_tt()/evaluate(), которые уже корректно учитывают
// var_to_level внутри core/common.hpp — здесь отдельная поправка не нужна.
class FallbackEngine {
public:
    static Result<Anf> compute(const Bdd& bdd, uint32_t n_vars) {
        auto tt_res = bdd.to_tt();
        if (!is_ok(tt_res)) {
            return fail<Anf>(error(tt_res).code, error(tt_res).message);
        }

        TruthTable tt = value(tt_res);
        auto& raw_tt = tt.raw();
        uint32_t n = raw_tt.num_vars();

        for (uint32_t i = 0; i < n; ++i) {
            uint32_t bit = 1u << i;
            uint32_t step = bit << 1;
            uint32_t limit = 1u << n;
            for (uint32_t j = 0; j < limit; j += step) {
                for (uint32_t k = 0; k < bit; ++k) {
                    // Быстрое преобразование Мёбиуса (TT -> коэффициенты
                    // Жегалкина): XOR нижней половины блока В верхнюю —
                    // направление проверено вручную для n=1 (см.
                    // bdd/README.md §4.3), обратный порядок давал
                    // [f(0)^f(1), f(1)] вместо верного [f(0), f(0)^f(1)].
                    if (kitty::get_bit(raw_tt, j + k)) {
                        kitty::flip_bit(raw_tt, j + k + bit);
                    }
                }
            }
        }

#if BMM_HAVE_BRIAL
        BoolePolyRing ring(n);
        BoolePolynomial poly(ring);
        uint64_t total_minterms = 1ULL << n;
        for (uint64_t i = 0; i < total_minterms; ++i) {
            if (kitty::get_bit(raw_tt, i)) {
                polybori::BooleExponent exp;
                for (uint32_t v = 0; v < n; ++v) {
                    if ((i >> v) & 1) {
                        exp.push_back(v);
                    }
                }
                poly += BoolePolynomial(exp, ring);
            }
        }
        return ok(Anf(std::move(poly), n));
#else
        AnfFallback fb;
        uint64_t total_minterms = 1ULL << n;
        for (uint64_t i = 0; i < total_minterms; ++i) {
            if (kitty::get_bit(raw_tt, i)) {
                std::vector<uint32_t> vars;
                for (uint32_t v = 0; v < n; ++v) {
                    if ((i >> v) & 1) {
                        vars.push_back(v);
                    }
                }
                fb.add_monomial(std::move(vars));
            }
        }
        return ok(Anf(std::move(fb), n));
#endif
    }
};

} // namespace detail

// ============================================================================
// Публичный интерфейс
// ============================================================================

Result<Anf> bdd_to_anf(const Bdd& bdd) {
    ZoneScoped;

    try {
        uint32_t n_vars = bdd.n_vars();
        const sylvan::Bdd f_syl = bdd.raw();

        if (f_syl.isZero()) {
#if BMM_HAVE_BRIAL
            BoolePolyRing ring(n_vars);
            return ok(Anf(BoolePolynomial(ring), n_vars));
#else
            return ok(Anf(AnfFallback(), n_vars));
#endif
        }
        if (f_syl.isOne()) {
#if BMM_HAVE_BRIAL
            BoolePolyRing ring(n_vars);
            return ok(Anf(ring.one(), n_vars));
#else
            AnfFallback fb;
            fb.add_monomial({});
            return ok(Anf(std::move(fb), n_vars));
#endif
        }

        detail::DenseBDD dense = detail::DenseBuilder::build(bdd);

        const size_t fallback_threshold = 50000;
        detail::ComputeResult result = detail::AnfEngine::compute(dense, n_vars, fallback_threshold);

        if (result.status == detail::ComputeStatus::FALLBACK_NEEDED) {
            return detail::FallbackEngine::compute(bdd, n_vars);
        }

#if BMM_HAVE_BRIAL
        return ok(Anf(std::move(result.poly), n_vars));
#else
        return ok(Anf(std::move(result.fallback), n_vars));
#endif

    } catch (const std::bad_alloc&) {
        return fail<Anf>(ErrorCode::OutOfMemory, "bdd_to_anf: исчерпана память");
    } catch (const std::exception& e) {
        return fail<Anf>(ErrorCode::InvalidArgument, std::string("bdd_to_anf error: ") + e.what());
    }
}

} // namespace bmm
