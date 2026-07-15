#include "ground_truth.hpp"

#include <random>

namespace bmm::verify {

namespace {

GroundTruthFunction make_constant(uint32_t n_vars, bool value) {
    GroundTruthFunction gt;
    gt.name = "const" + std::to_string(value) + "_n" + std::to_string(n_vars);
    gt.n_vars = n_vars;
    gt.table.assign(uint64_t{1} << n_vars, value);
    return gt;
}

GroundTruthFunction make_projection(uint32_t n_vars, uint32_t var_index) {
    GroundTruthFunction gt;
    gt.name = "proj_x" + std::to_string(var_index) + "_n" + std::to_string(n_vars);
    gt.n_vars = n_vars;
    const uint64_t rows = uint64_t{1} << n_vars;
    gt.table.resize(rows);
    for (uint64_t idx = 0; idx < rows; ++idx) gt.table[idx] = (idx >> var_index) & 1u;
    return gt;
}

GroundTruthFunction make_and_all(uint32_t n_vars) {
    GroundTruthFunction gt;
    gt.name = "and_all_n" + std::to_string(n_vars);
    gt.n_vars = n_vars;
    const uint64_t rows = uint64_t{1} << n_vars;
    const uint64_t all_ones = rows - 1;
    gt.table.resize(rows);
    for (uint64_t idx = 0; idx < rows; ++idx) gt.table[idx] = (idx == all_ones);
    return gt;
}

GroundTruthFunction make_or_all(uint32_t n_vars) {
    GroundTruthFunction gt;
    gt.name = "or_all_n" + std::to_string(n_vars);
    gt.n_vars = n_vars;
    const uint64_t rows = uint64_t{1} << n_vars;
    gt.table.resize(rows);
    for (uint64_t idx = 0; idx < rows; ++idx) gt.table[idx] = (idx != 0);
    return gt;
}

GroundTruthFunction make_parity(uint32_t n_vars) {
    GroundTruthFunction gt;
    gt.name = "xor_all_n" + std::to_string(n_vars);
    gt.n_vars = n_vars;
    const uint64_t rows = uint64_t{1} << n_vars;
    gt.table.resize(rows);
    for (uint64_t idx = 0; idx < rows; ++idx) {
        gt.table[idx] = (__builtin_popcountll(idx) % 2) != 0;
    }
    return gt;
}

GroundTruthFunction make_majority(uint32_t n_vars) {
    GroundTruthFunction gt;
    gt.name = "maj_n" + std::to_string(n_vars);
    gt.n_vars = n_vars;
    const uint64_t rows = uint64_t{1} << n_vars;
    gt.table.resize(rows);
    for (uint64_t idx = 0; idx < rows; ++idx) {
        gt.table[idx] = static_cast<uint32_t>(__builtin_popcountll(idx)) > n_vars / 2;
    }
    return gt;
}

GroundTruthFunction make_random(uint32_t n_vars, uint32_t seed) {
    GroundTruthFunction gt;
    gt.name = "random_n" + std::to_string(n_vars) + "_seed" + std::to_string(seed);
    gt.n_vars = n_vars;
    const uint64_t rows = uint64_t{1} << n_vars;
    gt.table.resize(rows);
    std::mt19937 rng(seed);
    std::bernoulli_distribution coin(0.5);
    for (uint64_t idx = 0; idx < rows; ++idx) gt.table[idx] = coin(rng);
    return gt;
}

}  // namespace

std::vector<GroundTruthFunction> growing_test_functions(uint32_t max_n) {
    std::vector<GroundTruthFunction> out;
    for (uint32_t n = 1; n <= max_n; ++n) {
        out.push_back(make_constant(n, false));
        out.push_back(make_constant(n, true));
        for (uint32_t v = 0; v < n; ++v) out.push_back(make_projection(n, v));
        out.push_back(make_and_all(n));
        out.push_back(make_or_all(n));
        out.push_back(make_parity(n));
        if (n % 2 == 1) out.push_back(make_majority(n));
        // Число случайных функций растёт медленнее числа точек: на больших n
        // перебор 2^n точек и так дорог, двух-трёх случайных сэмплов на n
        // достаточно, чтобы ловить баги, не завязанные на структурные частные
        // случаи (AND/OR/XOR/MAJ) выше.
        const uint32_t random_samples = n <= 8 ? 5 : 2;
        for (uint32_t s = 0; s < random_samples; ++s) out.push_back(make_random(n, n * 1000 + s));
    }
    return out;
}

std::optional<uint64_t> find_mismatch(
    const GroundTruthFunction& gt,
    const std::function<bool(const std::vector<bool>&)>& evaluate_under_test) {
    const uint64_t rows = uint64_t{1} << gt.n_vars;
    for (uint64_t idx = 0; idx < rows; ++idx) {
        const auto assignment = decode_assignment(idx, gt.n_vars);
        if (evaluate_under_test(assignment) != gt.evaluate(idx)) return idx;
    }
    return std::nullopt;
}

}  // namespace bmm::verify
