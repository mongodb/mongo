/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_SharedICRegisters_h
#define jit_SharedICRegisters_h

#if defined(JS_CODEGEN_X86)
#  include "jit/x86/SharedICRegisters-x86.h"
#elif defined(JS_CODEGEN_X64)
#  include "jit/x64/SharedICRegisters-x64.h"
#elif defined(JS_CODEGEN_ARM)
#  include "jit/arm/SharedICRegisters-arm.h"
#elif defined(JS_CODEGEN_ARM64)
#  include "jit/arm64/SharedICRegisters-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
#  include "jit/mips32/SharedICRegisters-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
#  include "jit/mips64/SharedICRegisters-mips64.h"
#elif defined(JS_CODEGEN_LOONG64)
#  include "jit/loong64/SharedICRegisters-loong64.h"
#elif defined(JS_CODEGEN_RISCV64)
#  include "jit/riscv64/SharedICRegisters-riscv64.h"
#elif defined(JS_CODEGEN_WASM32)
#  include "jit/wasm32/SharedICRegisters-wasm32.h"
#elif defined(JS_CODEGEN_NONE)
#  include "jit/none/SharedICRegisters-none.h"
#else
#  error "Unknown architecture!"
#endif

namespace js {
namespace jit {}  // namespace jit
}  // namespace js

#endif /* jit_SharedICRegisters_h */
