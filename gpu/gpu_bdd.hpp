#pragma once
#include <cstddef> // Добавлено: корректное определение size_t
#include <cstdint>

// Глобальная константа ограничений Truth Table перенесена ближе к типам данных
constexpr uint32_t kMaxTruthTableVars = 60;

struct alignas(16) DeviceBddNode {
    uint32_t level;
    uint32_t low;
    uint32_t high;
    uint32_t pad; 
};

struct GpuBddView {
    const DeviceBddNode* graph;
    uint32_t root;
    uint32_t nvars;
    uint32_t node_count;
    bool root_comp; 
};

class GpuBdd {
    DeviceBddNode* d_graph_ = nullptr;
    size_t num_nodes_ = 0;
    GpuBddView view_{};

public:
    GpuBdd(DeviceBddNode* d_graph, size_t num_nodes, GpuBddView view)
        : d_graph_(d_graph), num_nodes_(num_nodes), view_(view) {}

    ~GpuBdd(); 

    GpuBdd(const GpuBdd&) = delete;
    GpuBdd& operator=(const GpuBdd&) = delete;
    GpuBdd(GpuBdd&& other) noexcept;
    GpuBdd& operator=(GpuBdd&& other) noexcept;

    GpuBddView view() const noexcept { return view_; }
};