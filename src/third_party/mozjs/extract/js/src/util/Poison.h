/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Memory poisoning.
 */

#ifndef util_Poison_h
#define util_Poison_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/MemoryChecking.h"

#include <algorithm>  // std::min
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "jstypes.h"

#include "js/Value.h"
#include "util/DiagnosticAssertions.h"

/*
 * Allow extra GC poisoning to be enabled in crash-diagnostics and zeal
 * builds. Except in debug builds, this must be enabled by setting the
 * JSGC_EXTRA_POISONING environment variable.
 */
#if defined(JS_CRASH_DIAGNOSTICS) || defined(JS_GC_ZEAL)
#  define JS_GC_ALLOW_EXTRA_POISONING 1
#endif

namespace mozilla {

/**
 * Set the first |aNElem| T elements in |aDst| to |aSrc|.
 */
template <typename T>
static MOZ_ALWAYS_INLINE void PodSet(T* aDst, const T& aSrc, size_t aNElem) {
  for (const T* dstend = aDst + aNElem; aDst < dstend; ++aDst) {
    *aDst = aSrc;
  }
}

} /* namespace mozilla */

/*
 * Patterns used by SpiderMonkey to overwrite unused memory. If you are
 * accessing an object with one of these patterns, you probably have a dangling
 * pointer. These values should be odd.
 */
const uint8_t JS_FRESH_NURSERY_PATTERN = 0x2F;
const uint8_t JS_SWEPT_NURSERY_PATTERN = 0x2B;
const uint8_t JS_ALLOCATED_NURSERY_PATTERN = 0x2D;
const uint8_t JS_NOTINUSE_TRAILER_PATTERN = 0x43;
const uint8_t JS_FRESH_TENURED_PATTERN = 0x4F;
const uint8_t JS_MOVED_TENURED_PATTERN = 0x49;
const uint8_t JS_SWEPT_TENURED_PATTERN = 0x4B;
const uint8_t JS_ALLOCATED_TENURED_PATTERN = 0x4D;
const uint8_t JS_FREED_HEAP_PTR_PATTERN = 0x6B;
const uint8_t JS_FREED_CHUNK_PATTERN = 0x8B;
const uint8_t JS_FREED_ARENA_PATTERN = 0x9B;
const uint8_t JS_FRESH_MARK_STACK_PATTERN = 0x9F;
const uint8_t JS_RESET_VALUE_PATTERN = 0xBB;
const uint8_t JS_POISONED_JSSCRIPT_DATA_PATTERN = 0xDB;
const uint8_t JS_OOB_PARSE_NODE_PATTERN = 0xFF;
const uint8_t JS_LIFO_UNDEFINED_PATTERN = 0xcd;
const uint8_t JS_LIFO_UNINITIALIZED_PATTERN = 0xce;

// Even ones
const uint8_t JS_SCOPE_DATA_TRAILING_NAMES_PATTERN = 0xCC;

/*
 * Ensure JS_SWEPT_CODE_PATTERN is a byte pattern that will crash immediately
 * when executed, so either an undefined instruction or an instruction that's
 * illegal in user mode.
 */
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64) || \
    defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_WASM32)
#  define JS_SWEPT_CODE_PATTERN 0xED  // IN instruction, crashes in user mode.
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
#  define JS_SWEPT_CODE_PATTERN 0xA3  // undefined instruction
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
#  define JS_SWEPT_CODE_PATTERN 0x01  // undefined instruction
#elif defined(JS_CODEGEN_LOONG64)
#  define JS_SWEPT_CODE_PATTERN 0x01  // undefined instruction
#elif defined(JS_CODEGEN_RISCV64)
#  define JS_SWEPT_CODE_PATTERN \
    0x29  // illegal sb instruction, crashes in user mode.
#else
#  error "JS_SWEPT_CODE_PATTERN not defined for this platform"
#endif

enum class MemCheckKind : uint8_t {
  // Marks a region as poisoned. Memory sanitizers like ASan will crash when
  // accessing it (both reads and writes).
  MakeNoAccess,

  // Marks a region as having undefined contents. In ASan builds this just
  // unpoisons the memory. MSan and Valgrind can also use this to find
  // reads of uninitialized memory.
  MakeUndefined,
};

static MOZ_ALWAYS_INLINE void SetMemCheckKind(void* ptr, size_t bytes,
                                              MemCheckKind kind) {
  switch (kind) {
    case MemCheckKind::MakeUndefined:
      MOZ_MAKE_MEM_UNDEFINED(ptr, bytes);
      return;
    case MemCheckKind::MakeNoAccess:
      MOZ_MAKE_MEM_NOACCESS(ptr, bytes);
      return;
  }
  MOZ_CRASH("Invalid kind");
}

namespace js {

static inline void PoisonImpl(void* ptr, uint8_t value, size_t num) {
  // Without a valid Value tag, a poisoned Value may look like a valid
  // floating point number. To ensure that we crash more readily when
  // observing a poisoned Value, we make the poison an invalid ObjectValue.
  // Unfortunately, this adds about 2% more overhead, so we can only enable
  // it in debug.
#if defined(DEBUG)
  if (!num) {
    return;
  }

  uintptr_t poison;
  memset(&poison, value, sizeof(poison));
#  if defined(JS_PUNBOX64)
  poison = poison & ((uintptr_t(1) << JSVAL_TAG_SHIFT) - 1);
#  endif
  JS::Value v = js::PoisonedObjectValue(poison);

#  if defined(JS_NUNBOX32)
  // On 32-bit arch, `ptr` is 4 bytes aligned, and it's less than
  // `sizeof(JS::Value)` == 8 bytes.
  //
  // `mozilla::PodSet` with `v` requires the pointer to be 8 bytes aligned if
  // `value_count > 0`.
  //
  // If the pointer isn't 8 bytes aligned, fill the leading 1-4 bytes
  // separately here, so that either the pointer is 8 bytes aligned, or
  // we have no more bytes to fill.
  uintptr_t begin_count = std::min(num, uintptr_t(ptr) % sizeof(JS::Value));
  if (begin_count) {
    uint8_t* begin = static_cast<uint8_t*>(ptr);
    mozilla::PodSet(begin, value, begin_count);
    ptr = begin + begin_count;
    num -= begin_count;

    if (!num) {
      return;
    }
  }
#  endif

  MOZ_ASSERT(uintptr_t(ptr) % sizeof(JS::Value) == 0);

  size_t value_count = num / sizeof(v);
  size_t byte_count = num % sizeof(v);
  mozilla::PodSet(reinterpret_cast<JS::Value*>(ptr), v, value_count);
  if (byte_count) {
    uint8_t* bytes = static_cast<uint8_t*>(ptr);
    uint8_t* end = bytes + num;
    mozilla::PodSet(end - byte_count, value, byte_count);
  }
#else   // !DEBUG
  memset(ptr, value, num);
#endif  // !DEBUG
}

// Unconditionally poison a region on memory.
static inline void AlwaysPoison(void* ptr, uint8_t value, size_t num,
                                MemCheckKind kind) {
  PoisonImpl(ptr, value, num);
  SetMemCheckKind(ptr, num, kind);
}

#if defined(JS_GC_ALLOW_EXTRA_POISONING)
extern bool gExtraPoisoningEnabled;
#endif

// Conditionally poison a region of memory in debug builds and nightly builds
// when enabled by setting the JSGC_EXTRA_POISONING environment variable. Used
// by the GC in places where poisoning has a performance impact.
static inline void Poison(void* ptr, uint8_t value, size_t num,
                          MemCheckKind kind) {
#if defined(JS_GC_ALLOW_EXTRA_POISONING)
  if (js::gExtraPoisoningEnabled) {
    PoisonImpl(ptr, value, num);
  }
#endif
  SetMemCheckKind(ptr, num, kind);
}

// Poison a region of memory in debug builds. Can be disabled by setting the
// JSGC_EXTRA_POISONING environment variable.
static inline void DebugOnlyPoison(void* ptr, uint8_t value, size_t num,
                                   MemCheckKind kind) {
#if defined(DEBUG)
  Poison(ptr, value, num, kind);
#else
  SetMemCheckKind(ptr, num, kind);
#endif
}

}  // namespace js

#endif /* util_Poison_h */
