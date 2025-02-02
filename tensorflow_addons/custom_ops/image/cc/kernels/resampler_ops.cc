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

#define EIGEN_USE_THREADS

#include "tensorflow_addons/custom_ops/image/cc/kernels/resampler_ops.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/util/work_sharder.h"

namespace tensorflow {
namespace functor {

SamplingKernelType SamplingKernelTypeFromString(const StringPiece str) {
  const string lower_case = absl::AsciiStrToLower(str);
  if (lower_case == "lanczos1") return Lanczos1Kernel;
  if (lower_case == "lanczos3") return Lanczos3Kernel;
  if (lower_case == "lanczos5") return Lanczos5Kernel;
  if (lower_case == "gaussian") return GaussianKernel;
  if (lower_case == "box") return BoxKernel;
  if (lower_case == "triangle") return TriangleKernel;
  if (lower_case == "keyscubic") return KeysCubicKernel;
  if (lower_case == "mitchellcubic") return MitchellCubicKernel;
  return SamplingKernelTypeEnd;
}

}  // namespace functor
}  // namespace tensorflow



namespace tensorflow {

namespace addons {

using CPUDevice = Eigen::ThreadPoolDevice;
using GPUDevice = Eigen::GpuDevice;

namespace functor {

template <typename T>
struct Resampler2DFunctor<CPUDevice, T> {
  void operator()(OpKernelContext* ctx, const CPUDevice& d,
                  const T* __restrict__ data, const T* __restrict__ warp,
                  T* __restrict__ output, const int batch_size,
                  const int data_height, const int data_width,
                  const int data_channels, const int num_sampling_points,
                  const tensorflow::functor::SamplingKernelType kernel_type) {
    const int warp_batch_stride = num_sampling_points * 2;
    const int data_batch_stride = data_height * data_width * data_channels;
    const int output_batch_stride = num_sampling_points * data_channels;
    const T zero = static_cast<T>(0.0);
    const T one = static_cast<T>(1.0);

    // Creating the interpolation kernel
    auto kernel = tensorflow::functor::CreateKeysCubicKernel();

    auto resample_batches = [&](const int start, const int limit) {
      for (int batch_id = start; batch_id < limit; ++batch_id) {
        // Utility lambda to access data point and set output values.
        // The functions take care of performing the relevant pointer
        // arithmetics abstracting away the low level details in the
        // main loop over samples. Note that data is stored in NHWC format.
        auto set_output = [&](const int sample_id, const int channel,
                              const T value) {
          output[batch_id * output_batch_stride + sample_id * data_channels +
                 channel] = value;
        };

        auto get_data_point = [&](const int x, const int y, const int chan) {
          const bool point_is_in_range =
              (x >= 0 && y >= 0 && x <= data_width - 1 && y <= data_height - 1);
          return point_is_in_range
                     ? data[batch_id * data_batch_stride +
                            data_channels * (y * data_width + x) + chan]
                     : zero;
        };

        for (int sample_id = 0; sample_id < num_sampling_points; ++sample_id) {
          const T x = warp[batch_id * warp_batch_stride + sample_id * 2];
          const T y = warp[batch_id * warp_batch_stride + sample_id * 2 + 1];
          // The interpolation function:
          // a) implicitly pads the input data with 0s (hence the unusual checks
          // with {x,y} > -1)
          // b) returns 0 when sampling outside the (padded) image.
          // The effect is that the sampled signal smoothly goes to 0 outside
          // the original input domain, rather than presenting a jump
          // discontinuity at the image boundaries.
          if (x > static_cast<T>(-1.0) && y > static_cast<T>(-1.0) &&
              x < static_cast<T>(data_width) &&
              y < static_cast<T>(data_height)) {

            // Precompute floor (f) and ceil (c) values for x and y.
            const int fx = std::floor(static_cast<float>(x));
            const int fy = std::floor(static_cast<float>(y));

            const int span_size = static_cast<int>(std::ceil(kernel.Radius()));
            for (int chan = 0; chan < data_channels; ++chan) {
              T res = zero;

              for(int inx=-span_size; inx <= span_size; inx++){
                for(int iny=-span_size; iny <= span_size; iny++){        
                  const int cx = fx + inx;
                  const int cy = fy + iny;
                  const float dx = static_cast<float>(cx) - static_cast<float>(x);
                  const float dy = static_cast<float>(cy) - static_cast<float>(y);
                  res += get_data_point(cx, cy, chan) * static_cast<T>(kernel(dx) * kernel(dy));
                }
              }
              set_output(sample_id, chan, res);
            }

          } else {
            for (int chan = 0; chan < data_channels; ++chan) {
              set_output(sample_id, chan, zero);
            }
          }
        }
      }
    };
    // Rough estimate of work for each batch entry.
    // From third_party/tensorflow/core/util/work_sharder.cc we gather that an
    // estimate of the cost of each work unit is needed to correctly shard the
    // workload. thread_pool->ParallelFor assumes each cost unit is 1ns, minimum
    // cost per shard
    // being 10us.
    const int64 cost =
        static_cast<int64>(num_sampling_points) * data_channels * 1000;
    auto thread_pool = ctx->device()->tensorflow_cpu_worker_threads()->workers;
    thread_pool->ParallelFor(batch_size, cost, resample_batches);
  }
};

}  // namespace functor

template <typename Device, typename T>
class ResamplerOp : public OpKernel {
 public:
  explicit ResamplerOp(OpKernelConstruction* context) : OpKernel(context) {
    string kernel_type_str;
    OP_REQUIRES_OK(context, context->GetAttr("kernel_type", &kernel_type_str));
    kernel_type_ = tensorflow::functor::SamplingKernelTypeFromString(kernel_type_str);
    OP_REQUIRES(context, kernel_type_ != tensorflow::functor::SamplingKernelTypeEnd,
                errors::InvalidArgument("Unrecognized kernel type: " +
                                        kernel_type_str));
  }

  void Compute(OpKernelContext* ctx) override {
    const Tensor& data = ctx->input(0);
    const Tensor& warp = ctx->input(1);

    const TensorShape& data_shape = data.shape();
    OP_REQUIRES(ctx, data_shape.dims() == 4,
                errors::Unimplemented(
                    "Only bilinear interpolation is currently supported. The "
                    "input data shape must be [batch_size, data_height, "
                    "data_width, data_channels], but is: ",
                    data_shape.DebugString()));
    const TensorShape& warp_shape = warp.shape();
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsMatrixOrHigher(warp_shape),
        errors::InvalidArgument("warp should be at least a matrix, got shape ",
                                "warp should be at least a matrix, got shape ",
                                warp_shape.DebugString()));
    OP_REQUIRES(ctx, warp_shape.dim_size(warp_shape.dims() - 1) == 2,
                errors::Unimplemented(
                    "Only bilinear interpolation is supported, warping "
                    "coordinates must be 2D; warp shape last entry should be "
                    "2, but shape vector is: ",
                    warp_shape.DebugString()));
    OP_REQUIRES(ctx, data_shape.dim_size(0) == warp_shape.dim_size(0),
                errors::InvalidArgument(
                    "Batch size of data and warp tensor must be the same, but "
                    "input shapes are: ",
                    data_shape.DebugString(), ", ", warp_shape.DebugString()));
    const int batch_size = data_shape.dim_size(0);
    const int data_height = data_shape.dim_size(1);
    const int data_width = data_shape.dim_size(2);
    const int data_channels = data_shape.dim_size(3);
    TensorShape output_shape = warp.shape();
    output_shape.set_dim(output_shape.dims() - 1, data_channels);
    Tensor* output = nullptr;
    OP_REQUIRES_OK(ctx, ctx->allocate_output(0, output_shape, &output));

    // Execute kernel only for nonempty output; otherwise Eigen crashes on GPU.
    if (data.NumElements() > 0 && warp.NumElements() > 0) {
      const int num_sampling_points = warp.NumElements() / batch_size / 2;
      functor::Resampler2DFunctor<Device, T>()(
          ctx, ctx->eigen_device<Device>(), data.flat<T>().data(),
          warp.flat<T>().data(), output->flat<T>().data(), batch_size,
          data_height, data_width, data_channels, num_sampling_points, kernel_type_);
    }
  }

  tensorflow::functor::SamplingKernelType kernel_type_;
 private:
  TF_DISALLOW_COPY_AND_ASSIGN(ResamplerOp);
};

#define REGISTER(TYPE)                                                       \
  REGISTER_KERNEL_BUILDER(                                                   \
      Name("Addons>Resampler").Device(DEVICE_CPU).TypeConstraint<TYPE>("T"), \
      ResamplerOp<CPUDevice, TYPE>);

TF_CALL_half(REGISTER);
TF_CALL_float(REGISTER);
TF_CALL_double(REGISTER);
#undef REGISTER

#if GOOGLE_CUDA
#define REGISTER(TYPE)                                                       \
  REGISTER_KERNEL_BUILDER(                                                   \
      Name("Addons>Resampler").Device(DEVICE_GPU).TypeConstraint<TYPE>("T"), \
      ResamplerOp<GPUDevice, TYPE>)

TF_CALL_half(REGISTER);
TF_CALL_float(REGISTER);
TF_CALL_double(REGISTER);
#undef REGISTER
#endif  // GOOGLE_CUDA

namespace functor {

template <typename T>
struct ResamplerGrad2DFunctor<CPUDevice, T> {
  void operator()(OpKernelContext* ctx, const CPUDevice& d,
                  const T* __restrict__ data, const T* __restrict__ warp,
                  const T* __restrict__ grad_output, T* __restrict__ grad_data,
                  T* __restrict__ grad_warp, const int batch_size,
                  const int data_height, const int data_width,
                  const int data_channels, const int num_sampling_points) {
    // Set gradients to 0, because the kernel incrementally updates the
    // tensor entries by adding partial contributions.
    const int resampler_output_size =
        batch_size * num_sampling_points * data_channels;
    const int grad_warp_size = resampler_output_size / data_channels * 2;
    const int grad_data_size =
        data_height * data_width * data_channels * batch_size;
    memset(static_cast<void*>(grad_data), 0, sizeof(T) * grad_data_size);
    memset(static_cast<void*>(grad_warp), 0, sizeof(T) * grad_warp_size);

    const auto&& data_batch_stride = data_height * data_width * data_channels;
    const auto&& warp_batch_stride = num_sampling_points * 2;
    const int output_batch_stride = num_sampling_points * data_channels;
    const T zero = static_cast<T>(0.0);
    const T one = static_cast<T>(1.0);

    auto update_grads_for_batches = [&](const int start, const int limit) {
      for (int batch_id = start; batch_id < limit; ++batch_id) {
        // Utility lambdas to access data and update gradient tensors.
        // The functions take care of performing the relevant pointer
        // arithmetics abstracting away the low level details in the
        // main loop over samples. Note that data is stored in NHWC format.
        auto get_data_point = [&](const int x, const int y, const int chan) {
          const bool point_is_in_range =
              (x >= 0 && y >= 0 && x <= data_width - 1 && y <= data_height - 1);
          return point_is_in_range
                     ? data[batch_id * data_batch_stride +
                            data_channels * (y * data_width + x) + chan]
                     : zero;
        };

        auto update_grad_data = [&](const int x, const int y, const int chan,
                                    const T value) {
          const bool point_is_in_range =
              (x >= 0 && y >= 0 && x <= data_width - 1 && y <= data_height - 1);
          if (point_is_in_range) {
            grad_data[batch_id * data_batch_stride +
                      data_channels * (y * data_width + x) + chan] += value;
          }
        };

        auto update_grad_warp = [&](const int sample_id, const int channel,
                                    const T value) {
          grad_warp[batch_id * warp_batch_stride + sample_id * 2 + channel] +=
              value;
        };

        for (int sample_id = 0; sample_id < num_sampling_points; ++sample_id) {
          const T x = warp[batch_id * warp_batch_stride + sample_id * 2];
          const T y = warp[batch_id * warp_batch_stride + sample_id * 2 + 1];
          // The interpolation function whose gradient this function implements:
          // a) implicitly pads the input data with 0s (hence the unusual checks
          // with {x,y} > -1)
          // b) returns 0 when sampling outside the (padded) image.
          // The effect is that the sampled signal smoothly goes to 0 outside
          // the original input domain, rather than presenting a jump
          // discontinuity at the image boundaries.
          if (x > static_cast<T>(-1.0) && y > static_cast<T>(-1.0) &&
              x < static_cast<T>(data_width) &&
              y < static_cast<T>(data_height)) {
            // Precompute floor (f) and ceil (c) values for x and y.
            const int fx = std::floor(static_cast<float>(x));
            const int fy = std::floor(static_cast<float>(y));
            const int cx = fx + 1;
            const int cy = fy + 1;
            const T dx = static_cast<T>(cx) - x;
            const T dy = static_cast<T>(cy) - y;

            for (int chan = 0; chan < data_channels; ++chan) {
              const T grad_output_value =
                  grad_output[batch_id * output_batch_stride +
                              sample_id * data_channels + chan];
              const T img_fxfy = get_data_point(fx, fy, chan);
              const T img_cxcy = get_data_point(cx, cy, chan);
              const T img_fxcy = get_data_point(fx, cy, chan);
              const T img_cxfy = get_data_point(cx, fy, chan);

              // Update partial gradients wrt relevant warp field entries
              update_grad_warp(
                  sample_id, 0,
                  grad_output_value * ((one - dy) * (img_cxcy - img_fxcy) +
                                       dy * (img_cxfy - img_fxfy)));

              update_grad_warp(
                  sample_id, 1,
                  grad_output_value * ((one - dx) * (img_cxcy - img_cxfy) +
                                       dx * (img_fxcy - img_fxfy)));

              // Update partial gradients wrt sampled data
              update_grad_data(fx, fy, chan, grad_output_value * dx * dy);
              update_grad_data(cx, cy, chan,
                               grad_output_value * (one - dx) * (one - dy));
              update_grad_data(fx, cy, chan,
                               grad_output_value * dx * (one - dy));
              update_grad_data(cx, fy, chan,
                               grad_output_value * (one - dx) * dy);
            }
          }
        }
      }
    };
    // Rough estimate of work for each batch entry.
    // From third_party/tensorflow/core/util/work_sharder.cc we gather that an
    // estimate of the cost of each work unit is needed to correctly shard the
    // workload. thread_pool->ParallelFor assumes each cost unit is 1ns, minimum
    // cost per shard
    // being 10us.
    // TODO(fviola): Check out if there is a better way of doing this.
    auto thread_pool = ctx->device()->tensorflow_cpu_worker_threads()->workers;
    const int64 cost =
        static_cast<int64>(num_sampling_points) * data_channels * 1000;
    thread_pool->ParallelFor(batch_size, cost, update_grads_for_batches);
  }
};

}  // namespace functor

template <typename Device, typename T>
class ResamplerGradOp : public OpKernel {
 public:
  explicit ResamplerGradOp(OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(OpKernelContext* ctx) override {
    const Tensor& data = ctx->input(0);
    const Tensor& warp = ctx->input(1);
    const Tensor& grad_output = ctx->input(2);

    const TensorShape& data_shape = data.shape();
    OP_REQUIRES(ctx, data_shape.dims() == 4,
                errors::Unimplemented(
                    "Only bilinear interpolation is supported, the input data "
                    "tensor must be a batch of 2d data; data shape should have "
                    "4 entries corresponding to [batch_size, data_height, "
                    "data_width, data_channels], but is: ",
                    data_shape.DebugString()));
    const int batch_size = data_shape.dim_size(0);
    const int data_height = data_shape.dim_size(1);
    const int data_width = data_shape.dim_size(2);
    const int data_channels = data_shape.dim_size(3);
    const TensorShape& warp_shape = warp.shape();
    OP_REQUIRES(
        ctx, TensorShapeUtils::IsMatrixOrHigher(warp_shape),
        errors::InvalidArgument("warp should be at least a matrix, got shape ",
                                warp_shape.DebugString()));
    OP_REQUIRES(ctx, warp_shape.dim_size(warp_shape.dims() - 1) == 2,
                errors::Unimplemented(
                    "Only bilinear interpolation is supported, warping "
                    "coordinates must be 2D; warp shape last entry should be "
                    "2, but shape vector is: ",
                    warp_shape.DebugString()));
    const TensorShape& grad_output_shape = grad_output.shape();
    TensorShape resampler_output_shape = warp.shape();
    resampler_output_shape.set_dim(resampler_output_shape.dims() - 1,
                                   data_channels);
    OP_REQUIRES(ctx, grad_output_shape == resampler_output_shape,
                errors::InvalidArgument(
                    "grad_output shape is not consistent with data and warp "
                    "shapes; it should be ",
                    resampler_output_shape.DebugString(), " but is ",
                    grad_output_shape.DebugString()));
    Tensor* grad_data = nullptr;
    Tensor* grad_warp = nullptr;
    OP_REQUIRES_OK(ctx, ctx->allocate_output(0, data.shape(), &grad_data));
    OP_REQUIRES_OK(ctx, ctx->allocate_output(1, warp.shape(), &grad_warp));
    // Execute kernel only for nonempty output; otherwise Eigen crashes on GPU.
    if (data.NumElements() > 0 && warp.NumElements() > 0) {
      const int num_sampling_points = warp.NumElements() / batch_size / 2;
      functor::ResamplerGrad2DFunctor<Device, T>()(
          ctx, ctx->eigen_device<Device>(), data.flat<T>().data(),
          warp.flat<T>().data(), grad_output.flat<T>().data(),
          grad_data->flat<T>().data(), grad_warp->flat<T>().data(), batch_size,
          data_height, data_width, data_channels, num_sampling_points);
    }
  }

 private:
  TF_DISALLOW_COPY_AND_ASSIGN(ResamplerGradOp);
};

#define REGISTER(TYPE)                                    \
  REGISTER_KERNEL_BUILDER(Name("Addons>ResamplerGrad")    \
                              .Device(DEVICE_CPU)         \
                              .TypeConstraint<TYPE>("T"), \
                          ResamplerGradOp<CPUDevice, TYPE>);

TF_CALL_half(REGISTER);
TF_CALL_float(REGISTER);
TF_CALL_double(REGISTER);
#undef REGISTER

#if GOOGLE_CUDA
#define REGISTER(TYPE)                                    \
  REGISTER_KERNEL_BUILDER(Name("Addons>ResamplerGrad")    \
                              .Device(DEVICE_GPU)         \
                              .TypeConstraint<TYPE>("T"), \
                          ResamplerGradOp<GPUDevice, TYPE>)

TF_CALL_half(REGISTER);
TF_CALL_double(REGISTER);
TF_CALL_float(REGISTER);
#undef REGISTER
#endif  // GOOGLE_CUDA

}  // end namespace addons
}  // namespace tensorflow
