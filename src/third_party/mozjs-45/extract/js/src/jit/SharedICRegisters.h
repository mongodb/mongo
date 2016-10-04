/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_SharedICRegisters_h
#define jit_SharedICRegisters_h

#if defined(JS_CODEGEN_X86)
# include "jit/x86/SharedICRegisters-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/SharedICRegisters-x64.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/SharedICRegisters-arm.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/SharedICRegisters-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/SharedICRegisters-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
# include "jit/mips64/SharedICRegisters-mips64.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/SharedICRegisters-none.h"
#else
# error "Unknown architecture!"
#endif

namespace js {
namespace jit {

} // namespace jit
} // namespace js

#endif /* jit_SharedICRegisters_h */
