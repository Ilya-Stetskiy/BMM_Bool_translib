// benchmarks/large_scale_generators.hpp — генерация ВХОДНЫХ функций для
// бенчмарка масштабирования (benchmarks/large_scale_bench.cpp) БЕЗ
// материализации таблицы истинности (2^n). growing_test_functions
// (verify/ground_truth/ground_truth.hpp) для этой цели не годится: каждая
// функция там строит std::vector<bool> размера 2^n (см. make_constant/
// make_random и т.п.) — при n=100-2000, которые нужны здесь, это
// физически невозможно (2^100 не помещается ни в какую память). Здесь —
// три генератора, каждый строит функцию НАПРЯМУЮ в целевом представлении,
// стоимость O(n) или O(n_gates)/O(n_monomials), без единого прохода по
// точкам:
//   - random_aig: случайный AND-граф (аналог случайной схемы) через
//     mockturtle::create_and — тот же принцип, что и настоящие случайные
//     схемы (не структурно вырожденная цепочка), каждый новый гейт
//     ссылается на два случайных уже существующих сигнала.
//   - random_anf: случайный разреженный полином Жегалкина — n_monomials
//     случайных мономов ограниченной степени, тот же дух, что у реальных
//     ANF из настоящих S-box/хэш-функций (плотность << 2^n).
//   - random_thr: случайные веса + достижимый порог, O(n) — тот же метод,
//     что verify/reference_builders.cpp::growing_threshold_test_functions,
//     но без верхнего предела по n (там как побочный эффект вызывается с
//     max_n = kMaxGroundTruthVars = 12).
//
// Ни один из трёх генераторов не использует Assignment/2^n ни разу — сам
// факт, что n может быть 2000, а не 12, здесь ничего не стоит.

#pragma once

#include "core/anf_repr.hpp"
#include "core/common.hpp"

#include <mockturtle/networks/aig.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

namespace bmm::benchmarks {

// Случайная AIG-схема: n_vars входов, n_gates AND-гейтов. Каждый гейт —
// AND двух случайно выбранных уже существующих сигналов (PI или более
// ранний гейт) со случайной полярностью — обычный способ строить случайный
// DAG-граф без структурного вырождения (не цепочка/не дерево одной формы).
// PO — сигнал последнего гейта. Стоимость O(n_gates), НИКАКОГО обращения к
// таблице истинности.
inline Aig random_aig(uint32_t n_vars, uint32_t n_gates, uint64_t seed) {
    mockturtle::aig_network net;
    std::vector<mockturtle::aig_network::signal> pool;
    pool.reserve(n_vars + n_gates);
    for (uint32_t i = 0; i < n_vars; ++i) pool.push_back(net.create_pi());

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> polarity(0, 1);

    for (uint32_t g = 0; g < n_gates; ++g) {
        std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
        auto a = pool[pick(rng)];
        auto b = pool[pick(rng)];
        if (polarity(rng)) a = !a;
        if (polarity(rng)) b = !b;
        pool.push_back(net.create_and(a, b));
    }

    net.create_po(pool.back());
    return Aig(std::move(net));
}

// Случайный разреженный ANF: n_monomials мономов, степень каждого — от 1 до
// max_degree (равномерно), переменные монома — случайная выборка без
// повторов из [0, n_vars). Плотность (n_monomials относительно 2^n)
// контролируется явно вызывающим кодом — для реалистичного сравнения с
// реальными ANF (AES S-box, χ, разреженные корпуса persons.iis.nsk.su)
// n_monomials обычно берут << 2^n.
inline Anf random_anf(uint32_t n_vars, uint32_t n_monomials, uint32_t max_degree, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint32_t> degree_dist(1, std::min(max_degree, n_vars));
    std::vector<uint32_t> all_vars(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i) all_vars[i] = i;

#if BMM_HAVE_BRIAL
    BoolePolyRing ring(n_vars);
    BoolePolynomial poly(ring);
    for (uint32_t m = 0; m < n_monomials; ++m) {
        uint32_t degree = degree_dist(rng);
        std::vector<uint32_t> vars = all_vars;
        std::shuffle(vars.begin(), vars.end(), rng);
        vars.resize(degree);
        std::sort(vars.begin(), vars.end());

        BooleMonomial mono(ring);
        for (uint32_t v : vars) mono *= ring.variable(v);
        poly += mono;
    }
    return Anf(std::move(poly), n_vars);
#else
    AnfFallback poly;
    for (uint32_t m = 0; m < n_monomials; ++m) {
        uint32_t degree = degree_dist(rng);
        std::vector<uint32_t> vars = all_vars;
        std::shuffle(vars.begin(), vars.end(), rng);
        vars.resize(degree);
        std::sort(vars.begin(), vars.end());
        poly.add_monomial(std::move(vars));
    }
    return Anf(std::move(poly), n_vars);
#endif
}

// Случайная пороговая функция: веса — равномерно из [-weight_range,
// weight_range], порог — достижимая сумма (случайное подмножество весов +
// 1), тот же метод, что verify/reference_builders.cpp::
// growing_threshold_test_functions, но без завязки на kMaxGroundTruthVars —
// O(n_vars), ни одного обращения к 2^n.
inline Thr random_thr(uint32_t n_vars, int64_t weight_range, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int64_t> weight_dist(-weight_range, weight_range);

    std::vector<int64_t> weights(n_vars);
    for (auto& w : weights) w = weight_dist(rng);

    std::bernoulli_distribution coin(0.5);
    int64_t achievable = 0;
    for (int64_t w : weights) {
        if (coin(rng)) achievable += w;
    }

    return Thr(std::move(weights), achievable + 1);
}

}  // namespace bmm::benchmarks
