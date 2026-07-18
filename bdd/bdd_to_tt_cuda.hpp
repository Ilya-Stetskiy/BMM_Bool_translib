#pragma once
#include "../gpu/gpu_bdd.hpp" // Исправлено: выход из папки bdd в соседнюю папку gpu
#include <cstdint>
#include <cuda_runtime.h>

class ScratchArena {
    uint8_t* d_buffer_ = nullptr;
    size_t capacity_bytes_ = 0;

public:
    ~ScratchArena(); 
    uint64_t* allocate_words(size_t words);
};

class BddExecutionContext {
    cudaStream_t stream_ = nullptr;
    ScratchArena arena_;

public:
    explicit BddExecutionContext(cudaStream_t stream = nullptr) : stream_(stream) {}

    // NOTE: h_tt_out must point to pinned host memory (allocated via cudaMallocHost) 
    void evaluate(GpuBddView bdd_view, uint64_t* h_tt_out);
    
    cudaStream_t stream() const noexcept { return stream_; }
};