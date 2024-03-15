/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_SharedICHelpers_h
#define jit_SharedICHelpers_h

#if defined(JS_CODEGEN_X86)
#  include "jit/x86/SharedICHelpers-x86.h"
#elif defined(JS_CODEGEN_X64)
#  include "jit/x64/SharedICHelpers-x64.h"
#elif defined(JS_CODEGEN_ARM)
#  include "jit/arm/SharedICHelpers-arm.h"
#elif defined(JS_CODEGEN_ARM64)
#  include "jit/arm64/SharedICHelpers-arm64.h"
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
#  include "jit/mips-shared/SharedICHelpers-mips-shared.h"
#elif defined(JS_CODEGEN_LOONG64)
#  include "jit/loong64/SharedICHelpers-loong64.h"
#elif defined(JS_CODEGEN_RISCV64)
#  include "jit/riscv64/SharedICHelpers-riscv64.h"
#elif defined(JS_CODEGEN_WASM32)
#  include "jit/wasm32/SharedICHelpers-wasm32.h"
#elif defined(JS_CODEGEN_NONE)
#  include "jit/none/SharedICHelpers-none.h"
#else
#  error "Unknown architecture!"
#endif

namespace js {
namespace jit {}  // namespace jit
}  // namespace js

#endif /* jit_SharedICHelpers_h */
