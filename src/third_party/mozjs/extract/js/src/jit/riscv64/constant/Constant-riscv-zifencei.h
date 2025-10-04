// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef jit_riscv64_constant_Constant_riscv64_zifencei_h_
#define jit_riscv64_constant_Constant_riscv64_zifencei_h_

#include "jit/riscv64/constant/Base-constant-riscv.h"
namespace js {
namespace jit {
enum OpcodeRISCVIFENCEI : uint32_t {
  RO_FENCE_I = MISC_MEM | (0b001 << kFunct3Shift),
};
}
}  // namespace js
#endif  // jit_riscv64_constant_Constant_riscv64_zifencei_h_
