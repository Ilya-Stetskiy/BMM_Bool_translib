#include "bdd_kernel.hpp"
#include <cassert>

__global__ void bdd_evaluate_scalar_kernel(GpuBddView bdd, uint64_t* __restrict__ out_tt) {
    uint64_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint64_t total_threads = 1ULL << bdd.nvars; 
    
    if (idx >= total_threads) return;

    uint32_t curr = bdd.root;
    
    while (curr > 1) {
        assert(curr < bdd.node_count);
        
        const DeviceBddNode& node = bdd.graph[curr];
        bool bit = (idx >> node.level) & 1;
        curr = bit ? node.high : node.low;
    }

    bool result = (curr == 1);
    if (bdd.root_comp) result = !result;

    uint64_t word_idx = idx / 64;
    uint32_t bit_idx = idx % 64;
    
    if (result) {
        atomicOr(reinterpret_cast<unsigned long long*>(&out_tt[word_idx]), (1ULL << bit_idx));
    }
}

cudaError_t launch_bdd_evaluate_kernel(GpuBddView bdd, uint64_t* d_tt, cudaStream_t stream) {
    uint64_t total_threads = 1ULL << bdd.nvars;
    uint32_t block_size = 256;
    uint32_t grid_size = (total_threads + block_size - 1) / block_size;
    size_t words = total_threads / 64;
    
    cudaError_t err = cudaMemsetAsync(d_tt, 0, words * sizeof(uint64_t), stream);
    if (err != cudaSuccess) return err;
    
    bdd_evaluate_scalar_kernel<<<grid_size, block_size, 0, stream>>>(bdd, d_tt);
    
    // Исправлено: Не сбрасываем состояние ошибки, позволяя перехватить её на уровне API библиотеки
    return cudaPeekAtLastError();
}