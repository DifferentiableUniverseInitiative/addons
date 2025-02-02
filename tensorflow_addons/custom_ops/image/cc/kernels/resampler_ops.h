// Copyright 2019 The TensorFlow Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or  implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#ifndef TENSORFLOW_ADDONS_IMAGE_KERNELS_RESAMPLER_OPS_H_
#define TENSORFLOW_ADDONS_IMAGE_KERNELS_RESAMPLER_OPS_H_

#if PLATFORM_WINDOWS
#define __restrict__ __restrict
#endif

#include <string>

#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/kernels/image/sampling_kernels.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/strings/str_util.h"


namespace tensorflow {
namespace addons {
namespace functor {

// Helper functor for the Resampler Op in 2D
template <typename Device, typename T>
struct Resampler2DFunctor {
  void operator()(OpKernelContext* ctx, const Device& d,
                  const T* __restrict__ data, const T* __restrict__ warp,
                  T* __restrict__ output, const int batch_size,
                  const int data_height, const int data_width,
                  const int data_channels, const int num_sampling_points, 
                  const tensorflow::functor::SamplingKernelType kernel_type);
};

// Helper functor for the Resampler Gradient Op in 2D
template <typename Device, typename T>
struct ResamplerGrad2DFunctor {
  void operator()(OpKernelContext* ctx, const Device& d,
                  const T* __restrict__ data, const T* __restrict__ warp,
                  const T* __restrict__ grad_output, T* __restrict__ grad_data,
                  T* __restrict__ grad_warp, const int batch_size,
                  const int data_height, const int data_width,
                  const int data_channels, const int num_sampling_points);
};

}  // namespace functor
}  // namespace addons
}  // namespace tensorflow
#endif  // TENSORFLOW_ADDONS_IMAGE_KERNELS_RESAMPLER_OPS_H_