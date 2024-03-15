// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "jit/riscv64/extension/extension-riscv-f.h"
#include "jit/riscv64/Assembler-riscv64.h"
#include "jit/riscv64/constant/Constant-riscv64.h"
#include "jit/riscv64/Architecture-riscv64.h"
namespace js {
namespace jit {

// RV32F Standard Extension

void AssemblerRISCVF::flw(FPURegister rd, Register rs1, int16_t imm12) {
  GenInstrLoadFP_ri(0b010, rd, rs1, imm12);
}

void AssemblerRISCVF::fsw(FPURegister source, Register base, int16_t imm12) {
  GenInstrStoreFP_rri(0b010, base, source, imm12);
}

void AssemblerRISCVF::fmadd_s(FPURegister rd, FPURegister rs1, FPURegister rs2,
                              FPURegister rs3, FPURoundingMode frm) {
  GenInstrR4(0b00, MADD, rd, rs1, rs2, rs3, frm);
}

void AssemblerRISCVF::fmsub_s(FPURegister rd, FPURegister rs1, FPURegister rs2,
                              FPURegister rs3, FPURoundingMode frm) {
  GenInstrR4(0b00, MSUB, rd, rs1, rs2, rs3, frm);
}

void AssemblerRISCVF::fnmsub_s(FPURegister rd, FPURegister rs1, FPURegister rs2,
                               FPURegister rs3, FPURoundingMode frm) {
  GenInstrR4(0b00, NMSUB, rd, rs1, rs2, rs3, frm);
}

void AssemblerRISCVF::fnmadd_s(FPURegister rd, FPURegister rs1, FPURegister rs2,
                               FPURegister rs3, FPURoundingMode frm) {
  GenInstrR4(0b00, NMADD, rd, rs1, rs2, rs3, frm);
}

void AssemblerRISCVF::fadd_s(FPURegister rd, FPURegister rs1, FPURegister rs2,
                             FPURoundingMode frm) {
  GenInstrALUFP_rr(0b0000000, frm, rd, rs1, rs2);
}

void AssemblerRISCVF::fsub_s(FPURegister rd, FPURegister rs1, FPURegister rs2,
                             FPURoundingMode frm) {
  GenInstrALUFP_rr(0b0000100, frm, rd, rs1, rs2);
}

void AssemblerRISCVF::fmul_s(FPURegister rd, FPURegister rs1, FPURegister rs2,
                             FPURoundingMode frm) {
  GenInstrALUFP_rr(0b0001000, frm, rd, rs1, rs2);
}

void AssemblerRISCVF::fdiv_s(FPURegister rd, FPURegister rs1, FPURegister rs2,
                             FPURoundingMode frm) {
  GenInstrALUFP_rr(0b0001100, frm, rd, rs1, rs2);
}

void AssemblerRISCVF::fsqrt_s(FPURegister rd, FPURegister rs1,
                              FPURoundingMode frm) {
  GenInstrALUFP_rr(0b0101100, frm, rd, rs1, zero_reg);
}

void AssemblerRISCVF::fsgnj_s(FPURegister rd, FPURegister rs1,
                              FPURegister rs2) {
  GenInstrALUFP_rr(0b0010000, 0b000, rd, rs1, rs2);
}

void AssemblerRISCVF::fsgnjn_s(FPURegister rd, FPURegister rs1,
                               FPURegister rs2) {
  GenInstrALUFP_rr(0b0010000, 0b001, rd, rs1, rs2);
}

void AssemblerRISCVF::fsgnjx_s(FPURegister rd, FPURegister rs1,
                               FPURegister rs2) {
  GenInstrALUFP_rr(0b0010000, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVF::fmin_s(FPURegister rd, FPURegister rs1, FPURegister rs2) {
  GenInstrALUFP_rr(0b0010100, 0b000, rd, rs1, rs2);
}

void AssemblerRISCVF::fmax_s(FPURegister rd, FPURegister rs1, FPURegister rs2) {
  GenInstrALUFP_rr(0b0010100, 0b001, rd, rs1, rs2);
}

void AssemblerRISCVF::fcvt_w_s(Register rd, FPURegister rs1,
                               FPURoundingMode frm) {
  GenInstrALUFP_rr(0b1100000, frm, rd, rs1, zero_reg);
}

void AssemblerRISCVF::fcvt_wu_s(Register rd, FPURegister rs1,
                                FPURoundingMode frm) {
  GenInstrALUFP_rr(0b1100000, frm, rd, rs1, ToRegister(1));
}

void AssemblerRISCVF::fmv_x_w(Register rd, FPURegister rs1) {
  GenInstrALUFP_rr(0b1110000, 0b000, rd, rs1, zero_reg);
}

void AssemblerRISCVF::feq_s(Register rd, FPURegister rs1, FPURegister rs2) {
  GenInstrALUFP_rr(0b1010000, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVF::flt_s(Register rd, FPURegister rs1, FPURegister rs2) {
  GenInstrALUFP_rr(0b1010000, 0b001, rd, rs1, rs2);
}

void AssemblerRISCVF::fle_s(Register rd, FPURegister rs1, FPURegister rs2) {
  GenInstrALUFP_rr(0b1010000, 0b000, rd, rs1, rs2);
}

void AssemblerRISCVF::fclass_s(Register rd, FPURegister rs1) {
  GenInstrALUFP_rr(0b1110000, 0b001, rd, rs1, zero_reg);
}

void AssemblerRISCVF::fcvt_s_w(FPURegister rd, Register rs1,
                               FPURoundingMode frm) {
  GenInstrALUFP_rr(0b1101000, frm, rd, rs1, zero_reg);
}

void AssemblerRISCVF::fcvt_s_wu(FPURegister rd, Register rs1,
                                FPURoundingMode frm) {
  GenInstrALUFP_rr(0b1101000, frm, rd, rs1, ToRegister(1));
}

void AssemblerRISCVF::fmv_w_x(FPURegister rd, Register rs1) {
  GenInstrALUFP_rr(0b1111000, 0b000, rd, rs1, zero_reg);
}

#ifdef JS_CODEGEN_RISCV64
// RV64F Standard Extension (in addition to RV32F)

void AssemblerRISCVF::fcvt_l_s(Register rd, FPURegister rs1,
                               FPURoundingMode frm) {
  GenInstrALUFP_rr(0b1100000, frm, rd, rs1, ToRegister(2));
}

void AssemblerRISCVF::fcvt_lu_s(Register rd, FPURegister rs1,
                                FPURoundingMode frm) {
  GenInstrALUFP_rr(0b1100000, frm, rd, rs1, ToRegister(3));
}

void AssemblerRISCVF::fcvt_s_l(FPURegister rd, Register rs1,
                               FPURoundingMode frm) {
  GenInstrALUFP_rr(0b1101000, frm, rd, rs1, ToRegister(2));
}

void AssemblerRISCVF::fcvt_s_lu(FPURegister rd, Register rs1,
                                FPURoundingMode frm) {
  GenInstrALUFP_rr(0b1101000, frm, rd, rs1, ToRegister(3));
}
#endif

}  // namespace jit
}  // namespace js
