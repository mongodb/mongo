// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "jit/riscv64/extension/extension-riscv-zifencei.h"

#include "jit/riscv64/extension/base-assembler-riscv.h"
#include "jit/riscv64/constant/Constant-riscv64.h"
#include "jit/riscv64/Assembler-riscv64.h"
#include "jit/riscv64/Architecture-riscv64.h"
namespace js {
namespace jit {

void AssemblerRISCVZifencei::fence_i() {
  GenInstrI(0b001, MISC_MEM, ToRegister(0), ToRegister(0), 0);
}
}  // namespace jit
}  // namespace js
