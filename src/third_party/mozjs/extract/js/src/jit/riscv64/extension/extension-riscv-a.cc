// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "jit/riscv64/extension/extension-riscv-a.h"
#include "jit/riscv64/Assembler-riscv64.h"
#include "jit/riscv64/constant/Constant-riscv64.h"
#include "jit/riscv64/Architecture-riscv64.h"
namespace js {
namespace jit {

// RV32A Standard Extension
void AssemblerRISCVA::lr_w(bool aq, bool rl, Register rd, Register rs1) {
  GenInstrRAtomic(0b00010, aq, rl, 0b010, rd, rs1, zero_reg);
}

void AssemblerRISCVA::sc_w(bool aq, bool rl, Register rd, Register rs1,
                           Register rs2) {
  GenInstrRAtomic(0b00011, aq, rl, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVA::amoswap_w(bool aq, bool rl, Register rd, Register rs1,
                                Register rs2) {
  GenInstrRAtomic(0b00001, aq, rl, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVA::amoadd_w(bool aq, bool rl, Register rd, Register rs1,
                               Register rs2) {
  GenInstrRAtomic(0b00000, aq, rl, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVA::amoxor_w(bool aq, bool rl, Register rd, Register rs1,
                               Register rs2) {
  GenInstrRAtomic(0b00100, aq, rl, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVA::amoand_w(bool aq, bool rl, Register rd, Register rs1,
                               Register rs2) {
  GenInstrRAtomic(0b01100, aq, rl, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVA::amoor_w(bool aq, bool rl, Register rd, Register rs1,
                              Register rs2) {
  GenInstrRAtomic(0b01000, aq, rl, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVA::amomin_w(bool aq, bool rl, Register rd, Register rs1,
                               Register rs2) {
  GenInstrRAtomic(0b10000, aq, rl, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVA::amomax_w(bool aq, bool rl, Register rd, Register rs1,
                               Register rs2) {
  GenInstrRAtomic(0b10100, aq, rl, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVA::amominu_w(bool aq, bool rl, Register rd, Register rs1,
                                Register rs2) {
  GenInstrRAtomic(0b11000, aq, rl, 0b010, rd, rs1, rs2);
}

void AssemblerRISCVA::amomaxu_w(bool aq, bool rl, Register rd, Register rs1,
                                Register rs2) {
  GenInstrRAtomic(0b11100, aq, rl, 0b010, rd, rs1, rs2);
}

// RV64A Standard Extension (in addition to RV32A)
#ifdef JS_CODEGEN_RISCV64
void AssemblerRISCVA::lr_d(bool aq, bool rl, Register rd, Register rs1) {
  GenInstrRAtomic(0b00010, aq, rl, 0b011, rd, rs1, zero_reg);
}

void AssemblerRISCVA::sc_d(bool aq, bool rl, Register rd, Register rs1,
                           Register rs2) {
  GenInstrRAtomic(0b00011, aq, rl, 0b011, rd, rs1, rs2);
}

void AssemblerRISCVA::amoswap_d(bool aq, bool rl, Register rd, Register rs1,
                                Register rs2) {
  GenInstrRAtomic(0b00001, aq, rl, 0b011, rd, rs1, rs2);
}

void AssemblerRISCVA::amoadd_d(bool aq, bool rl, Register rd, Register rs1,
                               Register rs2) {
  GenInstrRAtomic(0b00000, aq, rl, 0b011, rd, rs1, rs2);
}

void AssemblerRISCVA::amoxor_d(bool aq, bool rl, Register rd, Register rs1,
                               Register rs2) {
  GenInstrRAtomic(0b00100, aq, rl, 0b011, rd, rs1, rs2);
}

void AssemblerRISCVA::amoand_d(bool aq, bool rl, Register rd, Register rs1,
                               Register rs2) {
  GenInstrRAtomic(0b01100, aq, rl, 0b011, rd, rs1, rs2);
}

void AssemblerRISCVA::amoor_d(bool aq, bool rl, Register rd, Register rs1,
                              Register rs2) {
  GenInstrRAtomic(0b01000, aq, rl, 0b011, rd, rs1, rs2);
}

void AssemblerRISCVA::amomin_d(bool aq, bool rl, Register rd, Register rs1,
                               Register rs2) {
  GenInstrRAtomic(0b10000, aq, rl, 0b011, rd, rs1, rs2);
}

void AssemblerRISCVA::amomax_d(bool aq, bool rl, Register rd, Register rs1,
                               Register rs2) {
  GenInstrRAtomic(0b10100, aq, rl, 0b011, rd, rs1, rs2);
}

void AssemblerRISCVA::amominu_d(bool aq, bool rl, Register rd, Register rs1,
                                Register rs2) {
  GenInstrRAtomic(0b11000, aq, rl, 0b011, rd, rs1, rs2);
}

void AssemblerRISCVA::amomaxu_d(bool aq, bool rl, Register rd, Register rs1,
                                Register rs2) {
  GenInstrRAtomic(0b11100, aq, rl, 0b011, rd, rs1, rs2);
}
#endif
}  // namespace jit
}  // namespace js
