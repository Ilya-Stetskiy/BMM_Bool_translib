#include "reference_builders.hpp"

#include <random>

#include "verify/metamorphic/metamorphic.hpp"
#include "verify/sat_encoding/sat_encoding.hpp"

namespace bmm::verify {

namespace {

uint64_t assignment_to_index(const std::vector<bool>& a) {
    uint64_t idx = 0;
    for (uint32_t i = 0; i < a.size(); ++i) {
        if (a[i]) idx |= (uint64_t{1} << i);
    }
    return idx;
}

Evaluator evaluator_from_gt(const GroundTruthFunction& gt) {
    return [&gt](const std::vector<bool>& a) { return gt.evaluate(assignment_to_index(a)); };
}

}  // namespace

Result<TruthTable> reference_truth_table(const GroundTruthFunction& gt) {
    if (gt.n_vars > kMaxTruthTableVars) return fail<TruthTable>(ErrorCode::TooManyVariables, "");
    TruthTable tt(gt.n_vars);
    for (uint64_t idx = 0; idx < gt.table.size(); ++idx) {
        if (gt.table[idx]) kitty::set_bit(tt.raw(), idx);
    }
    return ok<TruthTable>(std::move(tt));
}

Result<Aig> reference_aig(const GroundTruthFunction& gt) {
    if (gt.n_vars > kMaxReferenceAigVars) {
        return fail<Aig>(ErrorCode::TooManyVariables,
                          "reference_aig: n_vars > kMaxReferenceAigVars (MVP-ограничение, см. "
                          "sat_encoding.hpp)");
    }
    return ok<Aig>(Aig(reference_aig_from_table(gt.n_vars, evaluator_from_gt(gt))));
}

Result<Bdd> reference_bdd(const GroundTruthFunction& gt) {
    // Shannon/ITE-разложение через sylvan::Bdd::Ite (документированный,
    // фундаментальный примитив sylvan_obj.hpp) — механическое построение,
    // экспоненциальное по количеству узлов в худшем случае, но Sylvan
    // де-дуплицирует общие поддеревья через ROBDD-кэш, так что на практике
    // заметно дешевле, чем reference_aig (в отличие от AIG-дерева MUX здесь
    // нет отдельного построения на каждую ветку — Ite сам обращается к
    // унифицирующей таблице узлов).
    std::vector<bool> assignment(gt.n_vars, false);
    std::function<sylvan::Bdd(uint32_t)> build = [&](uint32_t depth) -> sylvan::Bdd {
        if (depth == gt.n_vars) {
            return gt.evaluate(assignment_to_index(assignment)) ? sylvan::Bdd::bddOne()
                                                                  : sylvan::Bdd::bddZero();
        }
        assignment[depth] = false;
        auto lo = build(depth + 1);
        assignment[depth] = true;
        auto hi = build(depth + 1);
        assignment[depth] = false;
        return sylvan::Bdd::bddVar(depth).Ite(hi, lo);
    };
    sylvan::Bdd root = gt.n_vars == 0
                            ? (gt.evaluate(0) ? sylvan::Bdd::bddOne() : sylvan::Bdd::bddZero())
                            : build(0);
    return ok<Bdd>(Bdd(root, gt.n_vars));
}

Result<Anf> reference_anf(const GroundTruthFunction& gt) {
    const auto coeffs = mobius_transform(gt.n_vars, evaluator_from_gt(gt));

#if BMM_HAVE_BRIAL
    // Не проверено вживую (см. предупреждение в reference_builders.hpp) —
    // ring создаётся заново на каждый вызов, чтобы не тащить состояние между
    // вызовами с разным n_vars.
    BoolePolyRing ring(gt.n_vars == 0 ? 1 : gt.n_vars);
    BoolePolynomial poly(ring);
    for (uint64_t mask = 0; mask < coeffs.size(); ++mask) {
        if (!coeffs[mask]) continue;
        BooleMonomial mono(ring);
        for (uint32_t i = 0; i < gt.n_vars; ++i) {
            if ((mask >> i) & 1u) mono *= ring.variable(i);
        }
        poly += mono;
    }
    return ok<Anf>(Anf(std::move(poly), gt.n_vars));
#else
    AnfFallback poly;
    for (uint64_t mask = 0; mask < coeffs.size(); ++mask) {
        if (!coeffs[mask]) continue;
        Monomial m;
        for (uint32_t i = 0; i < gt.n_vars; ++i) {
            if ((mask >> i) & 1u) m.push_back(i);
        }
        poly.add_monomial(std::move(m));
    }
    return ok<Anf>(Anf(std::move(poly), gt.n_vars));
#endif
}

std::vector<Thr> growing_threshold_test_functions(uint32_t max_n) {
    std::vector<Thr> out;
    for (uint32_t n = 1; n <= max_n; ++n) {
        // AND: все веса 1, theta = n.
        out.push_back(Thr(std::vector<int64_t>(n, 1), n));
        // OR: все веса 1, theta = 1.
        out.push_back(Thr(std::vector<int64_t>(n, 1), 1));
        // Majority (нечётный n): веса 1, theta = ceil(n/2).
        if (n % 2 == 1) out.push_back(Thr(std::vector<int64_t>(n, 1), (n + 1) / 2));
        // Взвешенная пороговая функция со случайными весами/порогом —
        // детерминированный seed для воспроизводимости CI.
        std::mt19937 rng(4242 + n);
        std::uniform_int_distribution<int64_t> weight_dist(-5, 5);
        std::vector<int64_t> weights(n);
        int64_t sum = 0;
        for (auto& w : weights) {
            w = weight_dist(rng);
            sum += w;
        }
        std::uniform_int_distribution<int64_t> theta_dist(-sum, sum);
        out.push_back(Thr(weights, theta_dist(rng)));
    }
    return out;
}

}  // namespace bmm::verify
