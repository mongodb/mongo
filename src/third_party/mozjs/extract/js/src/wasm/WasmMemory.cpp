/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmMemory.h"

#include "mozilla/MathAlgorithms.h"

#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "vm/ArrayBufferObject.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmProcess.h"

using mozilla::IsPowerOfTwo;

using namespace js;
using namespace js::wasm;

const char* wasm::ToString(IndexType indexType) {
  switch (indexType) {
    case IndexType::I32:
      return "i32";
    case IndexType::I64:
      return "i64";
    default:
      MOZ_CRASH();
  }
}

bool wasm::ToIndexType(JSContext* cx, HandleValue value, IndexType* indexType) {
  RootedString typeStr(cx, ToString(cx, value));
  if (!typeStr) {
    return false;
  }

  Rooted<JSLinearString*> typeLinearStr(cx, typeStr->ensureLinear(cx));
  if (!typeLinearStr) {
    return false;
  }

  if (StringEqualsLiteral(typeLinearStr, "i32")) {
    *indexType = IndexType::I32;
  } else if (StringEqualsLiteral(typeLinearStr, "i64")) {
    *indexType = IndexType::I64;
  } else {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_STRING_IDX_TYPE);
    return false;
  }
  return true;
}

/*
 * [SMDOC] Linear memory addresses and bounds checking
 *
 * (Also see "WASM Linear Memory structure" in vm/ArrayBufferObject.cpp)
 *
 *
 * Memory addresses, bounds check avoidance, and the huge memory trick.
 *
 * A memory address in an access instruction has three components, the "memory
 * base", the "pointer", and the "offset".  The "memory base" - the HeapReg on
 * most platforms and a value loaded from the instance on x86 - is a native
 * pointer that points to the start of the linear memory array; we'll ignore the
 * memory base in the following.  The "pointer" is the i32 or i64 index supplied
 * by the program as a separate value argument to the access instruction; it is
 * usually variable but can be constant.  The "offset" is a constant encoded in
 * the access instruction.
 *
 * The "effective address" (EA) is the non-overflowed sum of the pointer and the
 * offset (if the sum overflows the program traps); the pointer, offset, and EA
 * all have the same type, i32 or i64.
 *
 * An access has an "access size", which is the number of bytes that are
 * accessed - currently up to 16 (for V128).  The highest-addressed byte to be
 * accessed by an access is thus the byte at (pointer+offset+access_size-1),
 * where offset+access_size-1 is compile-time evaluable.
 *
 * Bounds checking ensures that the entire access is in bounds, ie, that the
 * highest-addressed byte is at an offset in the linear memory below that of the
 * memory's current byteLength.
 *
 * To avoid performing an addition with overflow check and a compare-and-branch
 * bounds check for every memory access, we use some tricks:
 *
 * - An access-protected guard region of size R at the end of each memory is
 *   used to trap accesses to out-of-bounds offsets in the range
 *   0..R-access_size.  Thus the offset and the access size need not be added
 *   into the pointer before the bounds check, saving the add and overflow
 *   check.  The offset is added into the pointer without an overflow check
 *   either directly before the access or in the access instruction itself
 *   (depending on the ISA).  The pointer must still be explicitly
 *   bounds-checked.
 *
 * - On 64-bit systems where we determine there is plenty of virtual memory
 *   space (and ideally we determine that the VM system uses overcommit), a
 *   32-bit memory is implemented as a 4GB + R reservation, where the memory
 *   from the current heap length through the end of the reservation is
 *   access-protected.  The protected area R allows offsets up to R-access_size
 *   to be encoded in the access instruction.  The pointer need not be bounds
 *   checked explicitly, since it has only a 4GB range and thus points into the
 *   4GB part of the reservation.  The offset can be added into the pointer
 *   (using 64-bit arithmetic) either directly before the access or in the
 *   access instruction.
 *
 * The value of R differs in the two situations; in the first case it tends to
 * be small, currently 64KB; in the second case it is large, currently 2GB+64KB.
 * The difference is due to explicit bounds checking tending to be used on
 * 32-bit systems where memory and address space are scarce, while the implicit
 * bounds check is used only on 64-bit systems after ensuring that sufficient
 * address space is available in the process.  (2GB is really overkill, and
 * there's nothing magic about it; we could use something much smaller.)
 *
 * The implicit bounds checking strategy with the large reservation is known
 * below and elsewhere as the "huge memory trick" or just "huge memory".
 *
 * All memories in a process use the same strategy, selected at process startup.
 * The immediate reason for that is that the machine code embeds the strategy
 * it's been compiled with, and may later be exposed to memories originating
 * from different modules or directly from JS.  If the memories did not all use
 * the same strategy, and the same strategy as the code, linking would fail or
 * we would have to recompile the code.
 *
 *
 * The boundsCheckLimit.
 *
 * The bounds check limit that is stored in the instance is always valid and is
 * always a 64-bit datum, and it is always correct to load it and use it as a
 * 64-bit value.  However, in situations when the 32 upper bits are known to be
 * zero, it is also correct to load just the low 32 bits from the address of the
 * limit (which is always little-endian when a JIT is enabled), and use that
 * value as the limit.
 *
 * On x86 and arm32 (and on any other 32-bit platform, should there ever be
 * one), there is explicit bounds checking and the heap, whether memory32 or
 * memory64, is limited to 2GB; the bounds check limit can be treated as a
 * 32-bit quantity.
 *
 * On all 64-bit platforms, we may use explicit bounds checking or the huge
 * memory trick for memory32, but must always use explicit bounds checking for
 * memory64.  If the heap does not have a known maximum size or the known
 * maximum is greater than or equal to 4GB, then the bounds check limit must be
 * treated as a 64-bit quantity; otherwise it can be treated as a 32-bit
 * quantity.
 *
 * On x64 and arm64 with Baseline and Ion, we allow 32-bit memories up to 4GB,
 * and 64-bit memories can be larger.
 *
 * On mips64, memories are limited to 2GB, for now.
 *
 * Asm.js memories are limited to 2GB even on 64-bit platforms, and we can
 * always assume a 32-bit bounds check limit for asm.js.
 *
 *
 * Constant pointers.
 *
 * If the pointer is constant then the EA can be computed at compile time, and
 * if the EA is below the initial memory size then the bounds check can be
 * elided.
 *
 *
 * Alignment checks.
 *
 * On all platforms, some accesses (currently atomics) require an alignment
 * check: the EA must be naturally aligned for the datum being accessed.
 * However, we do not need to compute the EA properly, we care only about the
 * low bits - a cheap, overflowing add is fine, and if the offset is known
 * to be aligned, only the pointer need be checked.
 */

// Bounds checks always compare the base of the memory access with the bounds
// check limit. If the memory access is unaligned, this means that, even if the
// bounds check succeeds, a few bytes of the access can extend past the end of
// memory. To guard against this, extra space is included in the guard region to
// catch the overflow. MaxMemoryAccessSize is a conservative approximation of
// the maximum guard space needed to catch all unaligned overflows.
//
// Also see "Linear memory addresses and bounds checking" above.

static const unsigned MaxMemoryAccessSize = LitVal::sizeofLargestValue();

// All plausible targets must be able to do at least IEEE754 double
// loads/stores, hence the lower limit of 8.  Some Intel processors support
// AVX-512 loads/stores, hence the upper limit of 64.
static_assert(MaxMemoryAccessSize >= 8, "MaxMemoryAccessSize too low");
static_assert(MaxMemoryAccessSize <= 64, "MaxMemoryAccessSize too high");
static_assert((MaxMemoryAccessSize & (MaxMemoryAccessSize - 1)) == 0,
              "MaxMemoryAccessSize is not a power of two");

#ifdef WASM_SUPPORTS_HUGE_MEMORY

static_assert(MaxMemoryAccessSize <= HugeUnalignedGuardPage,
              "rounded up to static page size");
static_assert(HugeOffsetGuardLimit < UINT32_MAX,
              "checking for overflow against OffsetGuardLimit is enough.");

// We have only tested huge memory on x64 and arm64.
#  if !(defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64))
#    error "Not an expected configuration"
#  endif

#endif

// On !WASM_SUPPORTS_HUGE_MEMORY platforms:
//  - To avoid OOM in ArrayBuffer::prepareForAsmJS, asm.js continues to use the
//    original ArrayBuffer allocation which has no guard region at all.
//  - For WebAssembly memories, an additional GuardSize is mapped after the
//    accessible region of the memory to catch folded (base+offset) accesses
//    where `offset < OffsetGuardLimit` as well as the overflow from unaligned
//    accesses, as described above for MaxMemoryAccessSize.

static const size_t OffsetGuardLimit = PageSize - MaxMemoryAccessSize;

static_assert(MaxMemoryAccessSize < GuardSize,
              "Guard page handles partial out-of-bounds");
static_assert(OffsetGuardLimit < UINT32_MAX,
              "checking for overflow against OffsetGuardLimit is enough.");

size_t wasm::GetMaxOffsetGuardLimit(bool hugeMemory) {
#ifdef WASM_SUPPORTS_HUGE_MEMORY
  return hugeMemory ? HugeOffsetGuardLimit : OffsetGuardLimit;
#else
  return OffsetGuardLimit;
#endif
}

// Assert that our minimum offset guard limit covers our inline
// memory.copy/fill optimizations.
static const size_t MinOffsetGuardLimit = OffsetGuardLimit;
static_assert(MaxInlineMemoryCopyLength < MinOffsetGuardLimit, "precondition");
static_assert(MaxInlineMemoryFillLength < MinOffsetGuardLimit, "precondition");

#ifdef JS_64BIT
wasm::Pages wasm::MaxMemoryPages(IndexType t) {
  MOZ_ASSERT_IF(t == IndexType::I64, !IsHugeMemoryEnabled(t));
  size_t desired = MaxMemoryLimitField(t);
  constexpr size_t actual = ArrayBufferObject::MaxByteLength / PageSize;
  return wasm::Pages(std::min(desired, actual));
}

size_t wasm::MaxMemoryBoundsCheckLimit(IndexType t) {
  return MaxMemoryPages(t).byteLength();
}

#else
// On 32-bit systems, the heap limit must be representable in the nonnegative
// range of an int32_t, which means the maximum heap size as observed by wasm
// code is one wasm page less than 2GB.
wasm::Pages wasm::MaxMemoryPages(IndexType t) {
  static_assert(ArrayBufferObject::MaxByteLength >= INT32_MAX / PageSize);
  return wasm::Pages(INT32_MAX / PageSize);
}

// The max bounds check limit can be larger than the MaxMemoryPages because it
// is really MaxMemoryPages rounded up to the next valid bounds check immediate,
// see ComputeMappedSize().
size_t wasm::MaxMemoryBoundsCheckLimit(IndexType t) {
  size_t boundsCheckLimit = size_t(INT32_MAX) + 1;
  MOZ_ASSERT(IsValidBoundsCheckImmediate(boundsCheckLimit));
  return boundsCheckLimit;
}
#endif

// Because ARM has a fixed-width instruction encoding, ARM can only express a
// limited subset of immediates (in a single instruction).

static const uint64_t HighestValidARMImmediate = 0xff000000;

//  Heap length on ARM should fit in an ARM immediate. We approximate the set
//  of valid ARM immediates with the predicate:
//    2^n for n in [16, 24)
//  or
//    2^24 * n for n >= 1.
bool wasm::IsValidARMImmediate(uint32_t i) {
  bool valid = (IsPowerOfTwo(i) || (i & 0x00ffffff) == 0);

  MOZ_ASSERT_IF(valid, i % PageSize == 0);

  return valid;
}

uint64_t wasm::RoundUpToNextValidARMImmediate(uint64_t i) {
  MOZ_ASSERT(i <= HighestValidARMImmediate);
  static_assert(HighestValidARMImmediate == 0xff000000,
                "algorithm relies on specific constant");

  if (i <= 16 * 1024 * 1024) {
    i = i ? mozilla::RoundUpPow2(i) : 0;
  } else {
    i = (i + 0x00ffffff) & ~0x00ffffff;
  }

  MOZ_ASSERT(IsValidARMImmediate(i));

  return i;
}

Pages wasm::ClampedMaxPages(IndexType t, Pages initialPages,
                            const Maybe<Pages>& sourceMaxPages,
                            bool useHugeMemory) {
  Pages clampedMaxPages;

  if (sourceMaxPages.isSome()) {
    // There is a specified maximum, clamp it to the implementation limit of
    // maximum pages
    clampedMaxPages = std::min(*sourceMaxPages, wasm::MaxMemoryPages(t));

#ifndef JS_64BIT
    static_assert(sizeof(uintptr_t) == 4, "assuming not 64 bit implies 32 bit");

    // On 32-bit platforms, prevent applications specifying a large max (like
    // MaxMemoryPages()) from unintentially OOMing the browser: they just want
    // "a lot of memory". Maintain the invariant that initialPages <=
    // clampedMaxPages.
    static const uint64_t OneGib = 1 << 30;
    static const Pages OneGibPages = Pages(OneGib >> wasm::PageBits);
    static_assert(HighestValidARMImmediate > OneGib,
                  "computing mapped size on ARM requires clamped max size");

    Pages clampedPages = std::max(OneGibPages, initialPages);
    clampedMaxPages = std::min(clampedPages, clampedMaxPages);
#endif
  } else {
    // There is not a specified maximum, fill it in with the implementation
    // limit of maximum pages
    clampedMaxPages = wasm::MaxMemoryPages(t);
  }

  // Double-check our invariants
  MOZ_RELEASE_ASSERT(sourceMaxPages.isNothing() ||
                     clampedMaxPages <= *sourceMaxPages);
  MOZ_RELEASE_ASSERT(clampedMaxPages <= wasm::MaxMemoryPages(t));
  MOZ_RELEASE_ASSERT(initialPages <= clampedMaxPages);

  return clampedMaxPages;
}

size_t wasm::ComputeMappedSize(wasm::Pages clampedMaxPages) {
  // Caller is responsible to ensure that clampedMaxPages has been clamped to
  // implementation limits.
  size_t maxSize = clampedMaxPages.byteLength();

  // It is the bounds-check limit, not the mapped size, that gets baked into
  // code. Thus round up the maxSize to the next valid immediate value
  // *before* adding in the guard page.
  //
  // Also see "Wasm Linear Memory Structure" in vm/ArrayBufferObject.cpp.
  uint64_t boundsCheckLimit = RoundUpToNextValidBoundsCheckImmediate(maxSize);
  MOZ_ASSERT(IsValidBoundsCheckImmediate(boundsCheckLimit));

  MOZ_ASSERT(boundsCheckLimit % gc::SystemPageSize() == 0);
  MOZ_ASSERT(GuardSize % gc::SystemPageSize() == 0);
  return boundsCheckLimit + GuardSize;
}

bool wasm::IsValidBoundsCheckImmediate(uint32_t i) {
#ifdef JS_CODEGEN_ARM
  return IsValidARMImmediate(i);
#else
  return true;
#endif
}

uint64_t wasm::RoundUpToNextValidBoundsCheckImmediate(uint64_t i) {
#ifdef JS_CODEGEN_ARM
  return RoundUpToNextValidARMImmediate(i);
#else
  return i;
#endif
}
