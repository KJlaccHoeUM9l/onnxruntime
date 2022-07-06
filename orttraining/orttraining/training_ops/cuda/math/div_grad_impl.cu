// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <cuda_runtime.h>
#include "core/providers/cuda/math/binary_elementwise_ops_impl.h"
#include "core/providers/cuda/cu_inc/common.cuh"
#include "core/providers/cuda/cu_inc/binary_elementwise_impl.cuh"
#include "core/framework/tensor_shape.h"

namespace onnxruntime {
namespace cuda {

// for now this operator classes are no different than a funciton.
// Eventually once multiple binary gradient ops are needed, we will pass
// its instance from API instead of direct function call.
template <class T>
struct OP_A_DivGrad {
  __device__ __inline__ T operator()(T dy, T b) const { return dy / b; }
};
template <class T>
struct OP_B_DivGrad {
  __device__ __inline__ T operator()(T dy, T a, T b) const { return -dy * a / (b * b); }
};

template <typename T, typename OffsetCalcT, bool require_da, bool require_db>
__global__ void UnrolledBinaryElementwiseDivGradKernel(const T* a_data, const T* b_data, const T* dy_data,
                                                       T* output_da_data, T* output_db_data, OffsetCalcT offset_calc,
                                                       CUDA_LONG N) {
  CUDA_LONG start = kElementsPerThread * kThreadsPerBlock * blockIdx.x + threadIdx.x;
  T avalue[kElementsPerThread];
  T bvalue[kElementsPerThread];

  CUDA_LONG id = start;
#pragma unroll
  for (int i = 0; i < kElementsPerThread; ++i) {
    if (id < N) {
      TArray<int32_t, 2> offsets = offset_calc.get(id);
      avalue[i] = a_data[offsets[0]];
      bvalue[i] = b_data[offsets[1]];
      id += kThreadsPerBlock;
    }
  }

  id = start;
#pragma unroll
  for (int i = 0; i < kElementsPerThread; ++i) {
    if (id < N) {
      if (require_da) output_da_data[id] = OP_A_DivGrad<T>()(dy_data[id], bvalue[i]);
      if (require_db) output_db_data[id] = OP_B_DivGrad<T>()(dy_data[id], avalue[i], bvalue[i]);
      id += kThreadsPerBlock;
    }
  }
}

#define HANDLE_DIVGRAD_REQUIREMENT()                                                                                \
  if (da_output_data && db_output_data)                                                                             \
    UnrolledBinaryElementwiseDivGradKernel<T, decltype(offset_calc), true, true>                                    \
        <<<blocks_per_grid, kThreadsPerBlock, 0, stream>>>(a_data, b_data, dy_data, da_output_data, db_output_data, \
                                                           offset_calc, N);                                         \
  else if (da_output_data)                                                                                          \
    UnrolledBinaryElementwiseDivGradKernel<T, decltype(offset_calc), true, false>                                   \
        <<<blocks_per_grid, kThreadsPerBlock, 0, stream>>>(a_data, b_data, dy_data, da_output_data, db_output_data, \
                                                           offset_calc, N);                                         \
  else                                                                                                              \
    UnrolledBinaryElementwiseDivGradKernel<T, decltype(offset_calc), true, false>                                   \
        <<<blocks_per_grid, kThreadsPerBlock, 0, stream>>>(a_data, b_data, dy_data, da_output_data, db_output_data, \
                                                           offset_calc, N)

#define HANDLE_DIVGRAD_CHANNEL_BATCH(IS_RHS_NEED_COMPUTE, IS_BATCH_N)                    \
  auto offset_calc = BinaryBatchOffsetCalculator<IS_RHS_NEED_COMPUTE, IS_BATCH_N>(h, c); \
  HANDLE_DIVGRAD_REQUIREMENT()

#define LAUNCH_DIVGRAD_PER_CHANNEL_KERNEL(is_rhs_need_compute, is_batch_n)                                         \
  auto offset_calc = BinaryPerChannelOffsetCalculator<is_rhs_need_compute, is_batch_n>(args.height, args.channel); \
  HANDLE_DIVGRAD_REQUIREMENT()

#define HANDLE_DIVGRAD_BATCH_PER_CHANNEL(is_rhs_need_compute)      \
  if (args.batch == 1) {                                           \
    LAUNCH_DIVGRAD_PER_CHANNEL_KERNEL(is_rhs_need_compute, false); \
  } else {                                                         \
    LAUNCH_DIVGRAD_PER_CHANNEL_KERNEL(is_rhs_need_compute, true);  \
  }

#define HANDLE_DIVGRAD_RHS_INDEX_TYPE(lhs_index_type, rhs_index_type)                       \
  case rhs_index_type: {                                                                    \
    auto offset_calc = BinaryOffsetCalculator<lhs_index_type, rhs_index_type>(              \
        static_cast<int>(args.rank), args.lhs_strides, args.rhs_strides, args.output_fdms); \
    HANDLE_DIVGRAD_REQUIREMENT();                                                           \
  } break

#define HANDLE_DIVGRAD_LHS_INDEX_TYPE(lhs_index_type, rhs_index_type_val)             \
  case lhs_index_type: {                                                              \
    switch (rhs_index_type_val) {                                                     \
      HANDLE_DIVGRAD_RHS_INDEX_TYPE(lhs_index_type, BroadcastIndexType::NoBroadcast); \
      HANDLE_DIVGRAD_RHS_INDEX_TYPE(lhs_index_type, BroadcastIndexType::Scalar);      \
      HANDLE_DIVGRAD_RHS_INDEX_TYPE(lhs_index_type, BroadcastIndexType::NeedCompute); \
    }                                                                                 \
  } break

template <typename T>
void ImplDivGrad(cudaStream_t stream, const T* a_data, const T* b_data, const T* dy_data, T* da_output_data,
                 T* db_output_data, const BinaryElementwiseArgs& args) {
  if (args.output_size == 0) return;
  CUDA_LONG N = static_cast<CUDA_LONG>(args.output_size);
  int blocks_per_grid = static_cast<int>(CeilDiv(N, kElementsPerThread * kThreadsPerBlock));
  if (args.per_channel_type == PerChannelType::LhsNeedCompute) {
    HANDLE_DIVGRAD_BATCH_PER_CHANNEL(false);
  } else if (args.per_channel_type == PerChannelType::RhsNeedCompute) {
    HANDLE_DIVGRAD_BATCH_PER_CHANNEL(true);
  } else {
    switch (args.lhs_index_type) {
      HANDLE_DIVGRAD_LHS_INDEX_TYPE(BroadcastIndexType::NoBroadcast, args.rhs_index_type);
      HANDLE_DIVGRAD_LHS_INDEX_TYPE(BroadcastIndexType::Scalar, args.rhs_index_type);
      HANDLE_DIVGRAD_LHS_INDEX_TYPE(BroadcastIndexType::NeedCompute, args.rhs_index_type);
    }
  }
}

#define SPECIALIZED_DIV_GRAD_IMPL(T)                                                                    \
  template void ImplDivGrad<T>(cudaStream_t stream, const T* a_data, const T* b_data, const T* dy_data, \
                               T* da_output_data, T* db_output_data, const BinaryElementwiseArgs& args);

SPECIALIZED_DIV_GRAD_IMPL(half)
SPECIALIZED_DIV_GRAD_IMPL(float)
SPECIALIZED_DIV_GRAD_IMPL(double)

}  // namespace cuda
}  // namespace onnxruntime
