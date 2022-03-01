/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Flush the instruction cache of instructions in an address range. */

#ifndef jit_FlushICache_h
#define jit_FlushICache_h

#include "mozilla/Assertions.h"  // MOZ_CRASH

#include <stddef.h>  // size_t

namespace js {
namespace jit {

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)

inline void FlushICache(void* code, size_t size,
                        bool codeIsThreadLocal = true) {
  // No-op. Code and data caches are coherent on x86 and x64.
}

#elif (defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)) || \
    (defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64))

extern void FlushICache(void* code, size_t size, bool codeIsThreadLocal = true);

#elif defined(JS_CODEGEN_NONE)

inline void FlushICache(void* code, size_t size,
                        bool codeIsThreadLocal = true) {
  MOZ_CRASH();
}

#else
#  error "Unknown architecture!"
#endif

}  // namespace jit
}  // namespace js

#endif  // jit_FlushICache_h
