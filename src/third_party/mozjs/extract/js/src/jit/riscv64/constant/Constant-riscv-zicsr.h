// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef jit_riscv64_constant_Constant_riscv64_zicsr_h_
#define jit_riscv64_constant_Constant_riscv64_zicsr_h_

#include "jit/riscv64/constant/Base-constant-riscv.h"
namespace js {
namespace jit {
// RISCV CSR related bit mask and shift
const int kFcsrFlagsBits = 5;
const uint32_t kFcsrFlagsMask = (1 << kFcsrFlagsBits) - 1;
const int kFcsrFrmBits = 3;
const int kFcsrFrmShift = kFcsrFlagsBits;
const uint32_t kFcsrFrmMask = ((1 << kFcsrFrmBits) - 1) << kFcsrFrmShift;
const int kFcsrBits = kFcsrFlagsBits + kFcsrFrmBits;
const uint32_t kFcsrMask = kFcsrFlagsMask | kFcsrFrmMask;

enum OpcodeRISCVZICSR : uint32_t {
  // RV32/RV64 Zicsr Standard Extension
  RO_CSRRW = SYSTEM | (0b001 << kFunct3Shift),
  RO_CSRRS = SYSTEM | (0b010 << kFunct3Shift),
  RO_CSRRC = SYSTEM | (0b011 << kFunct3Shift),
  RO_CSRRWI = SYSTEM | (0b101 << kFunct3Shift),
  RO_CSRRSI = SYSTEM | (0b110 << kFunct3Shift),
  RO_CSRRCI = SYSTEM | (0b111 << kFunct3Shift),
};
}  // namespace jit
}  // namespace js
#endif  // jit_riscv64_constant_Constant_riscv64_zicsr_h_
