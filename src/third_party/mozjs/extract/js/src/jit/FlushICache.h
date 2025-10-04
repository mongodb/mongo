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

inline void FlushICache(void* code, size_t size) {
  // No-op. Code and data caches are coherent on x86 and x64.
}

#elif (defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)) ||   \
    (defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)) || \
    defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_RISCV64)

// Invalidate the given code range from the icache. This will also flush the
// execution context for this core. If this code is to be executed on another
// thread, that thread must perform an execution context flush first using
// `FlushExecutionContext` below.
extern void FlushICache(void* code, size_t size);

#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)

inline void FlushICache(void* code, size_t size) { MOZ_CRASH(); }

#else
#  error "Unknown architecture!"
#endif

#if (defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)) ||       \
    (defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)) || \
    defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_RISCV64)

inline void FlushExecutionContext() {
  // No-op. Execution context is coherent with instruction cache.
}
inline bool CanFlushExecutionContextForAllThreads() { return true; }
inline void FlushExecutionContextForAllThreads() {
  // No-op. Execution context is coherent with instruction cache.
}

#elif defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)

inline void FlushExecutionContext() { MOZ_CRASH(); }
inline bool CanFlushExecutionContextForAllThreads() { MOZ_CRASH(); }
inline void FlushExecutionContextForAllThreads() { MOZ_CRASH(); }

#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)

// ARM and ARM64 must flush the instruction pipeline of the current core
// before executing newly JIT'ed code. This will remove any stale data from
// the pipeline that may have referenced invalidated instructions.
//
// `FlushICache` will perform this for the thread that compiles the code, but
// other threads that may execute the code are responsible to call
// this method.
extern void FlushExecutionContext();

// Some platforms can flush the excecution context for other threads using a
// syscall. This is required when JIT'ed code will be published to multiple
// threads without a synchronization point where a `FlushExecutionContext`
// could be inserted.
extern bool CanFlushExecutionContextForAllThreads();

// Flushes the execution context of all threads in this process, equivalent to
// running `FlushExecutionContext` on every thread.
//
// Callers must ensure `CanFlushExecutionContextForAllThreads` is true, or
// else this will crash.
extern void FlushExecutionContextForAllThreads();

#else
#  error "Unknown architecture!"
#endif

}  // namespace jit
}  // namespace js

#endif  // jit_FlushICache_h
