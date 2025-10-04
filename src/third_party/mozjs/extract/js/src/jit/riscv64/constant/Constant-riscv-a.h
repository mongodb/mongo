// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef jit_riscv64_constant_Constant_riscv64_a_h_
#define jit_riscv64_constant_Constant_riscv64_a_h_

#include "jit/riscv64/constant/Base-constant-riscv.h"
namespace js {
namespace jit {

enum OpcodeRISCVA : uint32_t {
  // RV32A Standard Extension
  RO_LR_W = AMO | (0b010 << kFunct3Shift) | (0b00010 << kFunct5Shift),
  RO_SC_W = AMO | (0b010 << kFunct3Shift) | (0b00011 << kFunct5Shift),
  RO_AMOSWAP_W = AMO | (0b010 << kFunct3Shift) | (0b00001 << kFunct5Shift),
  RO_AMOADD_W = AMO | (0b010 << kFunct3Shift) | (0b00000 << kFunct5Shift),
  RO_AMOXOR_W = AMO | (0b010 << kFunct3Shift) | (0b00100 << kFunct5Shift),
  RO_AMOAND_W = AMO | (0b010 << kFunct3Shift) | (0b01100 << kFunct5Shift),
  RO_AMOOR_W = AMO | (0b010 << kFunct3Shift) | (0b01000 << kFunct5Shift),
  RO_AMOMIN_W = AMO | (0b010 << kFunct3Shift) | (0b10000 << kFunct5Shift),
  RO_AMOMAX_W = AMO | (0b010 << kFunct3Shift) | (0b10100 << kFunct5Shift),
  RO_AMOMINU_W = AMO | (0b010 << kFunct3Shift) | (0b11000 << kFunct5Shift),
  RO_AMOMAXU_W = AMO | (0b010 << kFunct3Shift) | (0b11100 << kFunct5Shift),

#ifdef JS_CODEGEN_RISCV64
  // RV64A Standard Extension (in addition to RV32A)
  RO_LR_D = AMO | (0b011 << kFunct3Shift) | (0b00010 << kFunct5Shift),
  RO_SC_D = AMO | (0b011 << kFunct3Shift) | (0b00011 << kFunct5Shift),
  RO_AMOSWAP_D = AMO | (0b011 << kFunct3Shift) | (0b00001 << kFunct5Shift),
  RO_AMOADD_D = AMO | (0b011 << kFunct3Shift) | (0b00000 << kFunct5Shift),
  RO_AMOXOR_D = AMO | (0b011 << kFunct3Shift) | (0b00100 << kFunct5Shift),
  RO_AMOAND_D = AMO | (0b011 << kFunct3Shift) | (0b01100 << kFunct5Shift),
  RO_AMOOR_D = AMO | (0b011 << kFunct3Shift) | (0b01000 << kFunct5Shift),
  RO_AMOMIN_D = AMO | (0b011 << kFunct3Shift) | (0b10000 << kFunct5Shift),
  RO_AMOMAX_D = AMO | (0b011 << kFunct3Shift) | (0b10100 << kFunct5Shift),
  RO_AMOMINU_D = AMO | (0b011 << kFunct3Shift) | (0b11000 << kFunct5Shift),
  RO_AMOMAXU_D = AMO | (0b011 << kFunct3Shift) | (0b11100 << kFunct5Shift),
#endif  // JS_CODEGEN_RISCV64
};
}  // namespace jit
}  // namespace js

#endif  // jit_riscv64_constant_Constant_riscv64_a_h_
