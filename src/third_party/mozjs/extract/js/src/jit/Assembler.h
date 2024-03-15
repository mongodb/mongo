/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Assembler_h
#define jit_Assembler_h

#if defined(JS_CODEGEN_X86)
#  include "jit/x86/Assembler-x86.h"
#elif defined(JS_CODEGEN_X64)
#  include "jit/x64/Assembler-x64.h"
#elif defined(JS_CODEGEN_ARM)
#  include "jit/arm/Assembler-arm.h"
#elif defined(JS_CODEGEN_ARM64)
#  include "jit/arm64/Assembler-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
#  include "jit/mips32/Assembler-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
#  include "jit/mips64/Assembler-mips64.h"
#elif defined(JS_CODEGEN_LOONG64)
#  include "jit/loong64/Assembler-loong64.h"
#elif defined(JS_CODEGEN_RISCV64)
#  include "jit/riscv64/Assembler-riscv64.h"
#elif defined(JS_CODEGEN_WASM32)
#  include "jit/wasm32/Assembler-wasm32.h"
#elif defined(JS_CODEGEN_NONE)
#  include "jit/none/Assembler-none.h"
#else
#  error "Unknown architecture!"
#endif

#endif /* jit_Assembler_h */
