/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/gpu/amdgpu_compiler.h"

#include "tensorflow/compiler/xla/service/algebraic_simplifier.h"
#include "tensorflow/compiler/xla/service/call_inliner.h"
#include "tensorflow/compiler/xla/service/gpu/cusolver_rewriter.h"
#include "tensorflow/compiler/xla/service/gpu/gemm_rewriter.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_conv_algorithm_picker.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_conv_padding_legalization.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_conv_rewriter.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_layout_assignment.h"
#include "tensorflow/compiler/xla/service/gpu/llvm_gpu_backend/gpu_backend_lib.h"
#include "tensorflow/compiler/xla/service/gpu/reduction_degenerate_dim_remover.h"
#include "tensorflow/compiler/xla/service/gpu/reduction_dimension_grouper.h"
#include "tensorflow/compiler/xla/service/gpu/reduction_layout_normalizer.h"
#include "tensorflow/compiler/xla/service/gpu/target_constants.h"
#include "tensorflow/compiler/xla/service/gpu/tree_reduction_rewriter.h"
#include "tensorflow/compiler/xla/service/gpu/triangular_solve_rewriter.h"
#include "tensorflow/compiler/xla/service/hlo_constant_folding.h"
#include "tensorflow/compiler/xla/service/hlo_cse.h"
#include "tensorflow/compiler/xla/service/hlo_pass_fix.h"
#include "tensorflow/compiler/xla/service/hlo_pass_pipeline.h"
#include "tensorflow/compiler/xla/service/hlo_verifier.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_util.h"
#include "tensorflow/compiler/xla/service/tuple_simplifier.h"
#include "tensorflow/compiler/xla/stream_executor/rocm/rocm_platform_id.h"
#include "tensorflow/tsl/platform/rocm_rocdl_path.h"

namespace xla {
namespace gpu {

namespace {

// Returns the directory containing ROCm-Device-Libs files. This function is
// called in AMDGPUCompiler's constructor, so can't return an error. But
// AMDGPUCompiler::Compile will return an error when the wanted rocdl file
// doesn't exist in the folder this function returns.
std::string GetROCDLDir(const HloModuleConfig& config) {
  std::vector<std::string> potential_rocdl_dirs;
  const std::string datadir = config.debug_options().xla_gpu_cuda_data_dir();
  if (!datadir.empty()) {
    potential_rocdl_dirs.push_back(datadir);
  }
  potential_rocdl_dirs.push_back(tsl::RocdlRoot());

  // Tries all potential ROCDL directories in the order they are inserted.
  // Returns the first directory that exists in the file system.
  for (const std::string& potential_rocdl_dir : potential_rocdl_dirs) {
    if (tsl::Env::Default()->IsDirectory(potential_rocdl_dir).ok()) {
      VLOG(2) << "Found ROCm-Device-Libs dir " << potential_rocdl_dir;
      return potential_rocdl_dir;
    }
    VLOG(2) << "Unable to find potential ROCm-Device-Libs dir "
            << potential_rocdl_dir;
  }

  // Last resort: maybe in the current folder.
  return ".";
}

}  // namespace

Status AMDGPUCompiler::OptimizeHloConvolutionCanonicalization(
    HloModule* hlo_module, GpuVersion gpu_version,
    se::dnn::VersionInfo dnn_version,
    se::DeviceMemoryAllocator* device_allocator) {
  // Convert convolutions into CustomCalls to MIOpen, then canonicalize them
  // (PadInsertion).
  HloPassPipeline pipeline("conv_canonicalization");
  pipeline.AddInvariantCheckerDebug<HloVerifier>(
      /*layout_sensitive=*/false,
      /*allow_mixed_precision=*/false);
  pipeline.AddPass<GpusolverRewriter>();
  pipeline.AddPass<GpuConvRewriter>();
  pipeline.AddPass<GpuConvPaddingLegalization>();

  // The conv padding/vectorization passes which we need to get rid of.  They
  // also leave behind unnecessary tuple/get-tuple-element pairs that
  // TupleSimplifier fixes.
  pipeline.AddPass<CallInliner>();
  pipeline.AddPass<TupleSimplifier>();

  // tf2xla bridge, DepthwiseConvolutionConverter and GpuConvRewriter
  // introduces reshapes and transposes that can be eliminated using
  // AlgebraicSimplifier  We run algsimp to a fixed point.
  AlgebraicSimplifierOptions options;
  options.set_enable_conv_operand_swap(false);
  options.set_push_concat_to_consumers(false);
  pipeline.AddPass<HloPassFix<AlgebraicSimplifier>>(options);

  pipeline.AddPass<HloConstantFolding>();
  TF_RETURN_IF_ERROR(pipeline.Run(hlo_module).status());

  return OkStatus();
}

Status AMDGPUCompiler::OptimizeHloPostLayoutAssignment(
    HloModule* hlo_module, se::StreamExecutor* stream_exec,
    se::DeviceMemoryAllocator* device_allocator,
    const GpuTargetConfig& gpu_target_config,
    const AutotuneResults* autotune_results) {
  TF_RETURN_IF_ERROR(GpuCompiler::OptimizeHloPostLayoutAssignment(
      hlo_module, stream_exec, device_allocator, gpu_target_config,
      autotune_results));

  HloPassPipeline post_pipeline("AMDGPU post-layout_assignment");

  // Transform TriangularSolve ops into custom-calls, so we can add temp
  // memory.
  post_pipeline.AddPass<TriangularSolveRewriter>();

  TF_RETURN_IF_ERROR(post_pipeline.Run(hlo_module).status());

  return OkStatus();
}

AMDGPUCompiler::AMDGPUCompiler()
    : GpuCompiler(stream_executor::rocm::kROCmPlatformId,
                  amdgpu::TargetTriple(), amdgpu::DataLayout()) {}

GpuVersion AMDGPUCompiler::GetGpuVersion(se::StreamExecutor* stream_exec) {
  return stream_exec->GetDeviceDescription().rocm_compute_capability();
}

StatusOr<std::pair<std::string, std::vector<uint8_t>>>
AMDGPUCompiler::CompileTargetBinary(const HloModuleConfig& module_config,
                                    llvm::Module* llvm_module,
                                    GpuVersion gpu_version, bool relocatable,
                                    const HloModule* debug_module) {
  if (rocdl_dir_.empty()) {
    // Compute rocdl_dir_ just once and cache it in this member.
    rocdl_dir_ = GetROCDLDir(module_config);
  }

  if (relocatable) {
    return Unimplemented("relocatable target binary is not implemented");
  }

  std::vector<uint8_t> hsaco;
  {
    XLA_SCOPED_LOGGING_TIMER(
        "AMDGPUCompiler::CompileTargetBinary - CompileToHsaco");
    TF_ASSIGN_OR_RETURN(
        hsaco, amdgpu::CompileToHsaco(llvm_module, gpu_version, module_config,
                                      rocdl_dir_));
  }

  return std::pair<std::string, std::vector<uint8_t>>("", std::move(hsaco));
}

}  // namespace gpu
}  // namespace xla
