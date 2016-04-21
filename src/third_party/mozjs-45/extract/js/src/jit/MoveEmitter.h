/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MoveEmitter_h
#define jit_MoveEmitter_h

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
# include "jit/x86-shared/MoveEmitter-x86-shared.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/MoveEmitter-arm.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/MoveEmitter-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/MoveEmitter-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
# include "jit/mips64/MoveEmitter-mips64.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/MoveEmitter-none.h"
#else
# error "Unknown architecture!"
#endif

#endif /* jit_MoveEmitter_h */
