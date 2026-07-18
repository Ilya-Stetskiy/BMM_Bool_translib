#pragma once
#include "gpu_bdd.hpp"
#include <vector>

struct LogicalBddGraph {
    struct Node { uint32_t level; uint32_t low_idx; uint32_t high_idx; };
    std::vector<Node> nodes;
    uint32_t root_id;
    uint32_t n_vars;
    bool root_comp;
};

class GpuSerializer {
public:
    static GpuBdd serialize(uint64_t sylvan_root);

private:
    static LogicalBddGraph build_nodes(uint64_t sylvan_root);
    static std::vector<uint32_t> reorder_nodes(const LogicalBddGraph& ir);
    static GpuBdd emit_gpu(const LogicalBddGraph& ir, const std::vector<uint32_t>& permutation);
};