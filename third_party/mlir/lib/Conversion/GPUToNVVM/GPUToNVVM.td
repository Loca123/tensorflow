//==-- GPUToNVVM.td - GPU Ops to NVVM Patterns ---------------*- tablegen -*==//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// Defines Patterns to lower GPU ops to NVVM.
//
//===----------------------------------------------------------------------===//

#ifdef MLIR_CONVERSION_GPUTONVVM_TD
#else
#define MLIR_CONVERSION_GPUTONVVM_TD

#ifdef GPU_OPS
#else
include "mlir/Dialect/GPU/GPUOps.td"
#endif // GPU_OPS

#ifdef NVVMIR_OPS
#else
include "mlir/Dialect/LLVMIR/NVVMOps.td"
#endif // NVVMIR_OPS

def : Pat<(GPU_BarrierOp), (NVVM_Barrier0Op)>;

#endif // MLIR_CONVERSION_GPUTONVVM_TD
