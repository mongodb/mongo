// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file."
#ifndef jit_riscv64_extension_Extension_riscv_a_h_
#define jit_riscv64_extension_Extension_riscv_a_h_
#include "mozilla/Assertions.h"

#include <stdint.h>

#include "jit/riscv64/extension/base-assembler-riscv.h"
#include "jit/riscv64/Register-riscv64.h"
namespace js {
namespace jit {
class AssemblerRISCVA : public AssemblerRiscvBase {
  // RV32A Standard Extension
 public:
  void lr_w(bool aq, bool rl, Register rd, Register rs1);
  void sc_w(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amoswap_w(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amoadd_w(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amoxor_w(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amoand_w(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amoor_w(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amomin_w(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amomax_w(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amominu_w(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amomaxu_w(bool aq, bool rl, Register rd, Register rs1, Register rs2);

#ifdef JS_CODEGEN_RISCV64
  // RV64A Standard Extension (in addition to RV32A)
  void lr_d(bool aq, bool rl, Register rd, Register rs1);
  void sc_d(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amoswap_d(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amoadd_d(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amoxor_d(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amoand_d(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amoor_d(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amomin_d(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amomax_d(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amominu_d(bool aq, bool rl, Register rd, Register rs1, Register rs2);
  void amomaxu_d(bool aq, bool rl, Register rd, Register rs1, Register rs2);
#endif
};
}  // namespace jit
}  // namespace js
#endif  // jit_riscv64_extension_Extension_riscv_A_h_
