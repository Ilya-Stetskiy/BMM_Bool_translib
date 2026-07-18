#pragma once
#include <cuda_runtime.h>
#include <stdexcept>
#include <string>
#include <string_view>

inline void check_cuda(cudaError_t err, std::string_view context) {
    if (err != cudaSuccess) {
        throw std::runtime_error(std::string("CUDA Error during ") + std::string(context) + ": " + cudaGetErrorString(err));
    }
}