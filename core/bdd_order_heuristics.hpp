// core/bdd_order_heuristics.hpp — общие эвристики выбора физического порядка
// переменных Sylvan-уровня, не привязанные к конкретному входному
// представлению. Изначально написаны только для anf/anf_to_bdd.cpp (степень
// вершины + FORCE, Aloul/Markov/Sakallah 2003, см. историю находок в
// anf/README.md §3) — вынесены сюда, чтобы aig/aig_to_bdd.cpp мог
// использовать ТОТ ЖЕ проверенный алгоритм на своём собственном графе
// взаимодействия переменных (построенном по фанин-структуре AIG, а не по
// мономам ANF), не завися от anf/ (core/ не должен зависеть от aig/anf/bdd/
// thr/, а они не должны зависеть друг от друга — см. core/CONVENTIONS.md).
//
// Само по себе НЕ содержит ничего специфичного для Bdd/Sylvan — это чистая
// комбинаторика над графом на n вершинах. Кто вызывает — сам решает, как
// построить InteractionGraph (по мономам, по фанинам AND-гейтов, и т.п.) и
// как использовать результат (bddVar(rank[var]) + Bdd(root, n, rank), см.
// anf/anf_to_bdd.cpp за примером использования).

#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bmm {

enum class VariableOrderHeuristic {
    MinIndex,        // возрастающий индекс переменной (без учёта структуры)
    LengthFreqRank,  // по (макс. "длина сети" переменной, затем частота) — см. compute_rank_by_length_freq
    Degree,          // по степени в графе взаимодействия переменных
    Force,           // алгоритм FORCE (Aloul, Markov, Sakallah, 2003)
};

using VariableRank = std::vector<uint32_t>;  // rank[var] -> приоритет, меньше = разложить/расположить раньше

// Граф взаимодействия переменных: узлы — переменные [0, n_vars), ребро u-v —
// переменные встретились в одном "локальном термe" (моном ANF, общий AND-гейт
// AIG и т.п.), вес — число раз, когда это произошло. Список смежности
// (hash map на переменную), не плотная матрица n x n — дешевле при
// разреженной структуре (типичный случай).
struct InteractionGraph {
    std::vector<std::unordered_map<uint32_t, uint32_t>> adjacency;  // adjacency[v][u] = вес ребра v-u

    explicit InteractionGraph(uint32_t n_vars) : adjacency(n_vars) {}

    void add_edge(uint32_t a, uint32_t b, uint32_t weight = 1) {
        if (a == b) return;
        adjacency[a][b] += weight;
        adjacency[b][a] += weight;
    }
};

inline VariableRank identity_rank(uint32_t n_vars) {
    VariableRank rank(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i) rank[i] = i;
    return rank;
}

// Ранг по степени вершины в графе взаимодействия (число РАЗЛИЧНЫХ соседей) —
// чем больше, тем раньше разложить/расположить: переменная сильнее всего
// "запутана" с остальными.
inline VariableRank compute_rank_by_degree(const InteractionGraph& graph, uint32_t n_vars) {
    std::vector<uint32_t> order(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i) order[i] = i;

    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const size_t deg_a = graph.adjacency[a].size();
        const size_t deg_b = graph.adjacency[b].size();
        if (deg_a != deg_b) return deg_a > deg_b;
        return a < b;  // детерминированный tie-break
    });

    VariableRank rank(n_vars);
    for (uint32_t r = 0; r < n_vars; ++r) rank[order[r]] = r;
    return rank;
}

// Алгоритм FORCE (Aloul, Markov, Sakallah, "FORCE: A Fast and Easy-To-
// Implement Variable-Ordering Heuristic", GLSVLSI 2003): "центр тяжести"
// переменной v — взвешенное среднее позиций всех её соседей в графе
// взаимодействия (вес ребра = число совместных вхождений), N итераций,
// начальная позиция — исходный индекс. См. anf/README.md §3.5/§5.2 за
// эмпирическим сравнением с альтернативами (min_index/degree/length_freq) —
// FORCE везде не хуже, чем в 1.95x от лучшей эвристики на 5 разных семействах
// структур, тогда как у остальных минимаксное сожаление доходит до 8-12x.
inline VariableRank compute_rank_force(const InteractionGraph& graph, uint32_t n_vars,
                                        uint32_t iterations = 20) {
    std::vector<double> pos(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i) pos[i] = static_cast<double>(i);

    std::vector<double> next_pos(n_vars);

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        for (uint32_t v = 0; v < n_vars; ++v) {
            const auto& neighbors = graph.adjacency[v];
            if (neighbors.empty()) {
                next_pos[v] = pos[v];
                continue;
            }
            double weighted_sum = 0.0;
            uint64_t weight_total = 0;
            for (const auto& [u, weight] : neighbors) {
                weighted_sum += pos[u] * static_cast<double>(weight);
                weight_total += weight;
            }
            next_pos[v] = weighted_sum / static_cast<double>(weight_total);
        }
        pos.swap(next_pos);
    }

    std::vector<uint32_t> order(n_vars);
    for (uint32_t i = 0; i < n_vars; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (pos[a] != pos[b]) return pos[a] < pos[b];
        return a < b;
    });

    VariableRank rank(n_vars);
    for (uint32_t r = 0; r < n_vars; ++r) rank[order[r]] = r;
    return rank;
}

}  // namespace bmm
