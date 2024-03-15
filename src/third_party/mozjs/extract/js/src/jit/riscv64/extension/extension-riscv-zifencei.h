// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef jit_riscv64_extension_Extension_riscv_zifencei_h_
#define jit_riscv64_extension_Extension_riscv_zifencei_h_
#include "mozilla/Assertions.h"

#include <stdint.h>

#include "jit/riscv64/extension/base-assembler-riscv.h"
namespace js {
namespace jit {
class AssemblerRISCVZifencei : public AssemblerRiscvBase {
 public:
  void fence_i();
};
}  // namespace jit
}  // namespace js
#endif  // jit_riscv64_extension_Extension_riscv_zifencei_h_
