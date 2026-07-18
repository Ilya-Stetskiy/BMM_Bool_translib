#include "bdd_to_tt_cuda.hpp"
#include "../gpu/bdd_kernel.hpp" // Исправлено
#include "../gpu/cuda_utils.hpp"  // Исправлено
#include <string>

ScratchArena::~ScratchArena() { 
    if (d_buffer_) {
        cudaFree(d_buffer_);
    }
}

uint64_t* ScratchArena::allocate_words(size_t words) {
    size_t required_bytes = words * sizeof(uint64_t);
    
    if (capacity_bytes_ < required_bytes) {
        if (d_buffer_) {
            cudaFree(d_buffer_);
            d_buffer_ = nullptr;
        }
        
        size_t new_capacity = capacity_bytes_ == 0 ? 1024 : capacity_bytes_;
        while (new_capacity < required_bytes) {
            new_capacity *= 2;
        }
        
        check_cuda(cudaMalloc(&d_buffer_, new_capacity), "cudaMalloc arena allocation");
        capacity_bytes_ = new_capacity;
    }
    return reinterpret_cast<uint64_t*>(d_buffer_);
}

void BddExecutionContext::evaluate(GpuBddView bdd_view, uint64_t* h_tt_out) {
    if (bdd_view.nvars > kMaxTruthTableVars) {
        throw std::invalid_argument(
            "Cannot evaluate Truth Table: nvars (" + std::to_string(bdd_view.nvars) + 
            ") exceeds maximum supported size of " + std::to_string(kMaxTruthTableVars) + " variables."
        );
    }

    constexpr uint64_t kWordBits = 64;
    size_t words_needed = (1ULL << bdd_view.nvars) / kWordBits;
    uint64_t* d_scratch = arena_.allocate_words(words_needed);

    check_cuda(launch_bdd_evaluate_kernel(bdd_view, d_scratch, stream_), "kernel launch execution");

    check_cuda(
        cudaMemcpyAsync(h_tt_out, d_scratch, words_needed * sizeof(uint64_t), cudaMemcpyDeviceToHost, stream_),
        "cudaMemcpyAsync D2H truth table transfer"
    );
}