// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef jit_riscv64_constant_Constant_riscv64_f_h_
#define jit_riscv64_constant_Constant_riscv64_f_h_
#include "jit/riscv64/constant/Base-constant-riscv.h"
namespace js {
namespace jit {

enum OpcodeRISCVF : uint32_t {
  // RV32F Standard Extension
  RO_FLW = LOAD_FP | (0b010 << kFunct3Shift),
  RO_FSW = STORE_FP | (0b010 << kFunct3Shift),
  RO_FMADD_S = MADD | (0b00 << kFunct2Shift),
  RO_FMSUB_S = MSUB | (0b00 << kFunct2Shift),
  RO_FNMSUB_S = NMSUB | (0b00 << kFunct2Shift),
  RO_FNMADD_S = NMADD | (0b00 << kFunct2Shift),
  RO_FADD_S = OP_FP | (0b0000000 << kFunct7Shift),
  RO_FSUB_S = OP_FP | (0b0000100 << kFunct7Shift),
  RO_FMUL_S = OP_FP | (0b0001000 << kFunct7Shift),
  RO_FDIV_S = OP_FP | (0b0001100 << kFunct7Shift),
  RO_FSQRT_S = OP_FP | (0b0101100 << kFunct7Shift) | (0b00000 << kRs2Shift),
  RO_FSGNJ_S = OP_FP | (0b000 << kFunct3Shift) | (0b0010000 << kFunct7Shift),
  RO_FSGNJN_S = OP_FP | (0b001 << kFunct3Shift) | (0b0010000 << kFunct7Shift),
  RO_FSQNJX_S = OP_FP | (0b010 << kFunct3Shift) | (0b0010000 << kFunct7Shift),
  RO_FMIN_S = OP_FP | (0b000 << kFunct3Shift) | (0b0010100 << kFunct7Shift),
  RO_FMAX_S = OP_FP | (0b001 << kFunct3Shift) | (0b0010100 << kFunct7Shift),
  RO_FCVT_W_S = OP_FP | (0b1100000 << kFunct7Shift) | (0b00000 << kRs2Shift),
  RO_FCVT_WU_S = OP_FP | (0b1100000 << kFunct7Shift) | (0b00001 << kRs2Shift),
  RO_FMV = OP_FP | (0b1110000 << kFunct7Shift) | (0b000 << kFunct3Shift) |
           (0b00000 << kRs2Shift),
  RO_FEQ_S = OP_FP | (0b010 << kFunct3Shift) | (0b1010000 << kFunct7Shift),
  RO_FLT_S = OP_FP | (0b001 << kFunct3Shift) | (0b1010000 << kFunct7Shift),
  RO_FLE_S = OP_FP | (0b000 << kFunct3Shift) | (0b1010000 << kFunct7Shift),
  RO_FCLASS_S = OP_FP | (0b001 << kFunct3Shift) | (0b1110000 << kFunct7Shift),
  RO_FCVT_S_W = OP_FP | (0b1101000 << kFunct7Shift) | (0b00000 << kRs2Shift),
  RO_FCVT_S_WU = OP_FP | (0b1101000 << kFunct7Shift) | (0b00001 << kRs2Shift),
  RO_FMV_W_X = OP_FP | (0b000 << kFunct3Shift) | (0b1111000 << kFunct7Shift),

#ifdef JS_CODEGEN_RISCV64
  // RV64F Standard Extension (in addition to RV32F)
  RO_FCVT_L_S = OP_FP | (0b1100000 << kFunct7Shift) | (0b00010 << kRs2Shift),
  RO_FCVT_LU_S = OP_FP | (0b1100000 << kFunct7Shift) | (0b00011 << kRs2Shift),
  RO_FCVT_S_L = OP_FP | (0b1101000 << kFunct7Shift) | (0b00010 << kRs2Shift),
  RO_FCVT_S_LU = OP_FP | (0b1101000 << kFunct7Shift) | (0b00011 << kRs2Shift),
#endif  // JS_CODEGEN_RISCV64
};
}  // namespace jit
}  // namespace js

#endif  // jit_riscv64_constant_Constant_riscv64_f_h_
