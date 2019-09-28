/*
 The implementation of this file is based on embLayerNorm plugin in TensorRT demo:
 https://github.com/NVIDIA/TensorRT/tree/release/5.1/demo/BERT/
 
Copyright 2019 NVIDIA Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "layer_norm.cuh"
#include "embed_layer_norm_impl.h"

using namespace onnxruntime::cuda;
using namespace cub;

namespace onnxruntime {
namespace contrib {
namespace cuda {

#ifdef USE_CUDA_FP16

template <unsigned TPB>
__global__ void maskIdxKernelSmall(int sequence_length, const int* mask, int* mask_index) {
  using BlockReduce = cub::BlockReduce<int, TPB>;
  __shared__ typename BlockReduce::TempStorage temp_storage;

  // blockIdx.x is b
  const int offset = blockIdx.x * sequence_length; // batch strides of sequence_length

  cub::Min min;
  int threadData(sequence_length);

  const int idx = offset + threadIdx.x;
  if (threadIdx.x < sequence_length) {
    const int val = mask[idx];
    if (val == 0)  // masked position: report thread idx
    {
      threadData = threadIdx.x;
    }
  }

  const auto min_index = BlockReduce(temp_storage).Reduce(threadData, min);

  if (threadIdx.x == 0) {
    mask_index[blockIdx.x] = min_index;
  }
}

template <unsigned TPB>
__global__ void maskIdxKernel(int sequence_length, const int* mask, int* mask_index) {
  using BlockReduce = cub::BlockReduce<int, TPB>;
  __shared__ typename BlockReduce::TempStorage temp_storage;

  // blockIdx.x is b
  const int offset = blockIdx.x * sequence_length;  // batch strides of sequence_length

  cub::Min min;
  int threadData(sequence_length);

  for (int i = threadIdx.x; i < sequence_length; i += TPB) {
    const int idx = offset + i;
    const int val = mask[idx];
    if (val == 0)  // masked position: report thread idx
    {
      threadData = min(threadData, i);
    }
  }

  const auto min_index = BlockReduce(temp_storage).Reduce(threadData, min);

  if (threadIdx.x == 0) {
    mask_index[blockIdx.x] = min_index;
  }
}

inline int computeMaskIdx(cudaStream_t stream, const int sequence_length, const int batch_size, const int* mask, int* mask_index) {
  // Mask idx is of length batch_size and assumes the valid region is contiguous starting
  // from the beginning of the sequence

  // Assume n = batch_size x sequence_length
  if (sequence_length <= 32) {
    maskIdxKernelSmall<32><<<batch_size, 32, 0, stream>>>(sequence_length, mask, mask_index);
  } else if (sequence_length <= 128) {
    maskIdxKernelSmall<128><<<batch_size, 128, 0, stream>>>(sequence_length, mask, mask_index);
  } else if (sequence_length == 384) {
    maskIdxKernelSmall<384><<<batch_size, 384, 0, stream>>>(sequence_length, mask, mask_index);
  } else {
    maskIdxKernel<256><<<batch_size, 256, 0, stream>>>(sequence_length, mask, mask_index);
  }

  CUDA_CALL(cudaPeekAtLastError());

  return 0;
}

template <typename T, unsigned TPB>
__global__ void embLayerNormKernel(int hidden_size, const int* input_ids, const int* segment_ids, const float* beta, const float* gamma,
                                   const T* word_embedding, const T* position_embedding, const T* segment_embedding,
                                   T* output) {
  KeyValuePairSum pairSum;
  // 1. lookup word and token of the block
  // blockIdx.x = position in the sequence
  // blockIdx.y = batch
  // gridDim.x = sequence_length
  // gridDim.y = batch_size
  __shared__ int word_id;
  __shared__ int segment_id;

  const T rld = T(1.f) / T(hidden_size);
  const int sequence_position = blockIdx.y * gridDim.x + blockIdx.x;
  if (threadIdx.x == 0) {
    word_id = input_ids[sequence_position];
    segment_id = segment_ids[sequence_position];
  }
  __syncthreads();

  // 2. load pos/tok/word embeddings and add them toghether
  // offset into embeddings is given by word_id * hidden_size
  const int position_offset = blockIdx.x * hidden_size;
  const int word_offset = word_id * hidden_size;
  const int segment_offset = segment_id * hidden_size;
  // the output offset is given by b * (sequence_length * hidden_size) + s * hidden_size
  const int output_offset = sequence_position * hidden_size;

  cub::KeyValuePair<T, T> threadData(0, 0);

  for (int it = threadIdx.x; it < hidden_size; it += TPB) {
    const T w(word_embedding[word_offset + it]);
    const T t(segment_embedding[segment_offset + it]);
    const T p(position_embedding[position_offset + it]);
    const T val = w + t + p;

    output[output_offset + it] = val;
    const T rldval = rld * val;
    threadData = pairSum(threadData, cub::KeyValuePair<T, T>(rldval, rldval * val));
  }

  // 3. layer norm on the sum
  layerNorm<T, TPB>(threadData, hidden_size, output_offset, beta, gamma, output);
}

template <typename T>
void embSkipLayerNorm(cudaStream_t stream, int hidden_size, int batch_size, int sequence_length,
                     const int* input_ids, const int* segment_ids, const float* beta, const float* gamma,
                     const T* word_embedding, const T* position_embedding, const T* segment_embedding,
                     T* output) {
  constexpr int tpb = 256;
  const dim3 grid(sequence_length, batch_size, 1);
  const dim3 block(tpb, 1, 1);

  embLayerNormKernel<T, tpb>
      <<<grid, block, 0, stream>>>(hidden_size, input_ids, segment_ids, beta, gamma, word_embedding, position_embedding, segment_embedding, output);

  CUDA_CALL(cudaPeekAtLastError());
}

void launchEmbedLayerNormKernel(void* output,
                                void* mask_index,
                                const int* input_ids,
                                const int* segment_ids,
                                const int* input_mask,
                                const float* gamma,
                                const float* beta,
                                const void* word_embedding,
                                const void* position_embedding,
                                const void* segment_embedding,
                                const int hidden_size,
                                int batch_size,
                                int sequence_length,
                                const size_t element_size) {
  const cudaStream_t stream = nullptr; // default stream

  if (element_size == 2) {
    embSkipLayerNorm<half>(stream, hidden_size, batch_size, sequence_length, input_ids, segment_ids,
                           beta, gamma, reinterpret_cast<const half*>(word_embedding), reinterpret_cast<const half*>(position_embedding), reinterpret_cast<const half*>(segment_embedding),
                           reinterpret_cast<half*>(output));
  } else {
    embSkipLayerNorm<float>(stream, hidden_size, batch_size, sequence_length, input_ids, segment_ids,
                           beta, gamma, reinterpret_cast<const float*>(word_embedding), reinterpret_cast<const float*>(position_embedding), reinterpret_cast<const float*>(segment_embedding),
                           reinterpret_cast<float*>(output));
  }

  computeMaskIdx(stream, sequence_length, hidden_size, input_mask, static_cast<int*>(mask_index));
}
#endif
}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
