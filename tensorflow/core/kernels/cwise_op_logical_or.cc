/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/kernels/cwise_ops_common.h"

namespace tensorflow {
REGISTER_KERNEL_BUILDER(Name("LogicalOr").Device(DEVICE_CPU),
                        BinaryOp<CPUDevice, functor::logical_or<bool>>);
#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
#if !defined(MLIR_GENERATED_GPU_KERNELS_ENABLED)
REGISTER_KERNEL_BUILDER(Name("LogicalOr").Device(DEVICE_GPU),
                        BinaryOp<GPUDevice, functor::logical_or<bool>>);
#endif  // !defined(MLIR_GENERATED_GPU_KERNELS_ENABLED)
#endif
}  // namespace tensorflow
