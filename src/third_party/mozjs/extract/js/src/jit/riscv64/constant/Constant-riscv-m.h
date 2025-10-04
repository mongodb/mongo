// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef jit_riscv64_constant_Constant_riscv64_m_h_
#define jit_riscv64_constant_Constant_riscv64_m_h_

#include "jit/riscv64/constant/Base-constant-riscv.h"
namespace js {
namespace jit {

enum OpcodeRISCVM : uint32_t {
  // RV32M Standard Extension
  RO_MUL = OP | (0b000 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_MULH = OP | (0b001 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_MULHSU = OP | (0b010 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_MULHU = OP | (0b011 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_DIV = OP | (0b100 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_DIVU = OP | (0b101 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_REM = OP | (0b110 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_REMU = OP | (0b111 << kFunct3Shift) | (0b0000001 << kFunct7Shift),

#ifdef JS_CODEGEN_RISCV64
  // RV64M Standard Extension (in addition to RV32M)
  RO_MULW = OP_32 | (0b000 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_DIVW = OP_32 | (0b100 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_DIVUW = OP_32 | (0b101 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_REMW = OP_32 | (0b110 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
  RO_REMUW = OP_32 | (0b111 << kFunct3Shift) | (0b0000001 << kFunct7Shift),
#endif
};
}  // namespace jit
}  // namespace js

#endif  // jit_riscv64_extension_CONSTANT_RISCV_M_h_
