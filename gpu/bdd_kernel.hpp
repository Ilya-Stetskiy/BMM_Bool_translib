#pragma once
#include "gpu_bdd.hpp"
#include <cuda_runtime.h>

cudaError_t launch_bdd_evaluate_kernel(GpuBddView bdd, uint64_t* d_tt, cudaStream_t stream);