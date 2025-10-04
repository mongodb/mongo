// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef jit_riscv64_extension_Extension_riscv_zicsr_h_
#define jit_riscv64_extension_Extension_riscv_zicsr_h_
#include "mozilla/Assertions.h"

#include <stdint.h>

#include "jit/riscv64/extension/base-assembler-riscv.h"
#include "jit/riscv64/Register-riscv64.h"
namespace js {
namespace jit {

class AssemblerRISCVZicsr : public AssemblerRiscvBase {
 public:
  // CSR
  void csrrw(Register rd, ControlStatusReg csr, Register rs1);
  void csrrs(Register rd, ControlStatusReg csr, Register rs1);
  void csrrc(Register rd, ControlStatusReg csr, Register rs1);
  void csrrwi(Register rd, ControlStatusReg csr, uint8_t imm5);
  void csrrsi(Register rd, ControlStatusReg csr, uint8_t imm5);
  void csrrci(Register rd, ControlStatusReg csr, uint8_t imm5);

  // illegal_trap
  void illegal_trap(uint8_t code);

  // Read instructions-retired counter
  void rdinstret(Register rd) { csrrs(rd, csr_instret, zero_reg); }
  void rdinstreth(Register rd) { csrrs(rd, csr_instreth, zero_reg); }
  void rdcycle(Register rd) { csrrs(rd, csr_cycle, zero_reg); }
  void rdcycleh(Register rd) { csrrs(rd, csr_cycleh, zero_reg); }
  void rdtime(Register rd) { csrrs(rd, csr_time, zero_reg); }
  void rdtimeh(Register rd) { csrrs(rd, csr_timeh, zero_reg); }

  void csrr(Register rd, ControlStatusReg csr) { csrrs(rd, csr, zero_reg); }
  void csrw(ControlStatusReg csr, Register rs) { csrrw(zero_reg, csr, rs); }
  void csrs(ControlStatusReg csr, Register rs) { csrrs(zero_reg, csr, rs); }
  void csrc(ControlStatusReg csr, Register rs) { csrrc(zero_reg, csr, rs); }

  void csrwi(ControlStatusReg csr, uint8_t imm) { csrrwi(zero_reg, csr, imm); }
  void csrsi(ControlStatusReg csr, uint8_t imm) { csrrsi(zero_reg, csr, imm); }
  void csrci(ControlStatusReg csr, uint8_t imm) { csrrci(zero_reg, csr, imm); }

  void frcsr(Register rd) { csrrs(rd, csr_fcsr, zero_reg); }
  void fscsr(Register rd, Register rs) { csrrw(rd, csr_fcsr, rs); }
  void fscsr(Register rs) { csrrw(zero_reg, csr_fcsr, rs); }

  void frrm(Register rd) { csrrs(rd, csr_frm, zero_reg); }
  void fsrm(Register rd, Register rs) { csrrw(rd, csr_frm, rs); }
  void fsrm(Register rs) { csrrw(zero_reg, csr_frm, rs); }

  void frflags(Register rd) { csrrs(rd, csr_fflags, zero_reg); }
  void fsflags(Register rd, Register rs) { csrrw(rd, csr_fflags, rs); }
  void fsflags(Register rs) { csrrw(zero_reg, csr_fflags, rs); }
};
}  // namespace jit
}  // namespace js
#endif  // jit_riscv64_extension_Extension_riscv_zicsr_h_
