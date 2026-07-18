#include "gpu_serializer.hpp"
#include "cuda_utils.hpp"
#include <limits>
#include <cassert>
#include <stdexcept>

GpuBdd::~GpuBdd() { 
    if (d_graph_) {
        cudaFree(d_graph_); 
    }
}

// Исправлено: Полное и безопасное зануление объекта-источника при Move-семантике
GpuBdd::GpuBdd(GpuBdd&& other) noexcept 
    : d_graph_(other.d_graph_), num_nodes_(other.num_nodes_), view_(other.view_) { 
    other.d_graph_ = nullptr; 
    other.num_nodes_ = 0;
    other.view_ = GpuBddView{};
}

GpuBdd& GpuBdd::operator=(GpuBdd&& other) noexcept {
    if (this != &other) {
        if (d_graph_) cudaFree(d_graph_);
        d_graph_ = other.d_graph_; 
        num_nodes_ = other.num_nodes_; 
        view_ = other.view_;
        
        other.d_graph_ = nullptr;
        other.num_nodes_ = 0;
        other.view_ = GpuBddView{};
    }
    return *this;
}

LogicalBddGraph GpuSerializer::build_nodes(uint64_t sylvan_root) {
    LogicalBddGraph ir{};
    // [Код обхода Sylvan DAG]
    return ir;
}

std::vector<uint32_t> GpuSerializer::reorder_nodes(const LogicalBddGraph& ir) {
    std::vector<uint32_t> permutation;
    if (ir.nodes.empty()) return permutation;

    std::vector<bool> visited(ir.nodes.size(), false);
    auto dfs = [&](auto& self, uint32_t idx) -> void {
        if (idx <= 1 || visited[idx]) return;
        visited[idx] = true;
        self(self, ir.nodes[idx].low_idx);
        self(self, ir.nodes[idx].high_idx);
        permutation.push_back(idx);
    };
    dfs(dfs, ir.root_id);
    return permutation;
}

GpuBdd GpuSerializer::emit_gpu(const LogicalBddGraph& ir, const std::vector<uint32_t>& permutation) {
    // Исправлено: Защита от пустого/невалидного графа Sylvan до обращения к индексам
    if (ir.nodes.size() < 2) {
        throw std::invalid_argument("Invalid BDD: Graph must contain at least terminal nodes 0 and 1.");
    }

    size_t active_nodes_count = 2 + permutation.size();
    std::vector<DeviceBddNode> h_graph(active_nodes_count);

    constexpr uint32_t kInvalidNode = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> forward_map(ir.nodes.size(), kInvalidNode);
    
    forward_map[0] = 0;
    forward_map[1] = 1;

    uint32_t new_id = 2;
    for (uint32_t old_id : permutation) {
        forward_map[old_id] = new_id++;
    }

    for (size_t old_id = 0; old_id < ir.nodes.size(); ++old_id) {
        if (old_id > 1 && forward_map[old_id] == kInvalidNode) continue; 
        
        // Исправлено: Ассерты для контроля корректности связей внутри DAG в Debug-сборке
        assert(ir.nodes[old_id].low_idx < ir.nodes.size());
        assert(ir.nodes[old_id].high_idx < ir.nodes.size());

        uint32_t target_id = forward_map[old_id];
        h_graph[target_id].level = ir.nodes[old_id].level;
        h_graph[target_id].low   = forward_map[ir.nodes[old_id].low_idx];
        h_graph[target_id].high  = forward_map[ir.nodes[old_id].high_idx];
    }

    DeviceBddNode* d_graph = nullptr;
    size_t allocation_size = active_nodes_count * sizeof(DeviceBddNode);

    try {
        check_cuda(cudaMalloc(&d_graph, allocation_size), "cudaMalloc graph allocation");
        check_cuda(cudaMemcpy(d_graph, h_graph.data(), allocation_size, cudaMemcpyHostToDevice), "cudaMemcpy graph host-to-device");
    }
    catch (...) {
        if (d_graph) cudaFree(d_graph);
        throw;
    }

    GpuBddView view{d_graph, forward_map[ir.root_id], ir.n_vars, static_cast<uint32_t>(active_nodes_count), ir.root_comp};
    return GpuBdd(d_graph, active_nodes_count, view);
}

GpuBdd GpuSerializer::serialize(uint64_t sylvan_root) {
    auto ir = build_nodes(sylvan_root);
    auto permutation = reorder_nodes(ir);
    return emit_gpu(ir, permutation);
}