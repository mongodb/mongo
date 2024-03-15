// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef jit_riscv64_extension_Extension_riscv_m_h_
#define jit_riscv64_extension_Extension_riscv_m_h_
#include "mozilla/Assertions.h"

#include <stdint.h>

#include "jit/riscv64/extension/base-assembler-riscv.h"
#include "jit/riscv64/Register-riscv64.h"
namespace js {
namespace jit {
class AssemblerRISCVM : public AssemblerRiscvBase {
  // RV32M Standard Extension
 public:
  void mul(Register rd, Register rs1, Register rs2);
  void mulh(Register rd, Register rs1, Register rs2);
  void mulhsu(Register rd, Register rs1, Register rs2);
  void mulhu(Register rd, Register rs1, Register rs2);
  void div(Register rd, Register rs1, Register rs2);
  void divu(Register rd, Register rs1, Register rs2);
  void rem(Register rd, Register rs1, Register rs2);
  void remu(Register rd, Register rs1, Register rs2);
#ifdef JS_CODEGEN_RISCV64
  // RV64M Standard Extension (in addition to RV32M)
  void mulw(Register rd, Register rs1, Register rs2);
  void divw(Register rd, Register rs1, Register rs2);
  void divuw(Register rd, Register rs1, Register rs2);
  void remw(Register rd, Register rs1, Register rs2);
  void remuw(Register rd, Register rs1, Register rs2);
#endif
};
}  // namespace jit
}  // namespace js
#endif  // jit_riscv64_extension_Extension_riscv_M_h_
