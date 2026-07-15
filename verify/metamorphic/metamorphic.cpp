#include "metamorphic.hpp"

#include <bit>

namespace bmm::verify {

namespace {

std::vector<bool> decode(uint64_t index, uint32_t n_vars) {
    std::vector<bool> assignment(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i) assignment[i] = (index >> i) & 1u;
    return assignment;
}

}  // namespace

ChowParameters compute_chow_parameters(uint32_t n_vars, const Evaluator& f) {
    ChowParameters cp;
    cp.correlations.assign(n_vars, 0);
    const uint64_t rows = uint64_t{1} << n_vars;
    for (uint64_t idx = 0; idx < rows; ++idx) {
        if (!f(decode(idx, n_vars))) continue;
        ++cp.weight;
        for (uint32_t i = 0; i < n_vars; ++i) {
            if ((idx >> i) & 1u) ++cp.correlations[i];
        }
    }
    return cp;
}

std::optional<ChowMismatch> compare_chow_parameters(const ChowParameters& a,
                                                     const ChowParameters& b) {
    if (a.weight != b.weight) return ChowMismatch{true, std::nullopt};
    for (uint32_t i = 0; i < a.correlations.size(); ++i) {
        if (a.correlations[i] != b.correlations[i]) return ChowMismatch{false, i};
    }
    return std::nullopt;
}

std::vector<bool> mobius_transform(uint32_t n_vars, const Evaluator& f) {
    const uint64_t rows = uint64_t{1} << n_vars;
    std::vector<uint8_t> a(rows);  // uint8_t, не vector<bool>: нужен произвольный доступ по ссылке для XOR ниже
    for (uint64_t idx = 0; idx < rows; ++idx) a[idx] = f(decode(idx, n_vars)) ? 1 : 0;

    for (uint32_t i = 0; i < n_vars; ++i) {
        for (uint64_t mask = 0; mask < rows; ++mask) {
            if ((mask >> i) & 1u) a[mask] ^= a[mask ^ (uint64_t{1} << i)];
        }
    }
    return std::vector<bool>(a.begin(), a.end());
}

uint32_t anf_degree(uint32_t n_vars, const Evaluator& f) {
    const auto coeffs = mobius_transform(n_vars, f);
    uint32_t degree = 0;
    for (uint64_t mask = 0; mask < coeffs.size(); ++mask) {
        if (coeffs[mask]) degree = std::max<uint32_t>(degree, std::popcount(mask));
    }
    return degree;
}

std::optional<DegreeMismatch> compare_anf_degree(uint32_t n_vars, const Evaluator& a,
                                                  const Evaluator& b) {
    const uint32_t da = anf_degree(n_vars, a);
    const uint32_t db = anf_degree(n_vars, b);
    if (da != db) return DegreeMismatch{da, db};
    return std::nullopt;
}

std::optional<CofactorMismatch> check_cofactor_commutativity(uint32_t n_vars, const Evaluator& a,
                                                               const Evaluator& b, uint32_t var_i,
                                                               uint32_t var_j) {
    // Свободные переменные — все, кроме var_i/var_j; перебираем их отдельным
    // индексом free_index и восстанавливаем полное присвоение raw_index —
    // сознательно другой путь построения индекса, чем decode() выше/в
    // ground_truth::decode_assignment, чтобы проверка не свелась к тому же
    // перебору с тем же кодом.
    std::vector<uint32_t> free_vars;
    for (uint32_t v = 0; v < n_vars; ++v) {
        if (v != var_i && v != var_j) free_vars.push_back(v);
    }
    const uint64_t free_rows = uint64_t{1} << free_vars.size();

    for (bool vi : {false, true}) {
        for (bool vj : {false, true}) {
            for (uint64_t free_index = 0; free_index < free_rows; ++free_index) {
                std::vector<bool> assignment(n_vars, false);
                assignment[var_i] = vi;
                assignment[var_j] = vj;
                for (uint32_t k = 0; k < free_vars.size(); ++k) {
                    assignment[free_vars[k]] = (free_index >> k) & 1u;
                }
                if (a(assignment) != b(assignment)) {
                    return CofactorMismatch{var_i, var_j, vi, vj, free_index};
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace bmm::verify
