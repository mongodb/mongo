/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/MathAlgorithms.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <tuple>
#include <utility>

#include "jit/AtomicOperations.h"
#include "js/GCAPI.h"

#if defined(__arm__)
#  include "jit/arm/Architecture-arm.h"
#endif

#ifdef JS_HAVE_GENERATED_ATOMIC_OPS

using namespace js;
using namespace js::jit;

// A "block" is a sequence of bytes that is a reasonable quantum to copy to
// amortize call overhead when implementing memcpy and memmove.  A block will
// not fit in registers on all platforms and copying it without using
// intermediate memory will therefore be sensitive to overlap.
//
// A "word" is an item that we can copy using only register intermediate storage
// on all platforms; words can be individually copied without worrying about
// overlap.
//
// Blocks and words can be aligned or unaligned; specific (generated) copying
// functions handle this in platform-specific ways.

static constexpr size_t WORDSIZE = sizeof(uintptr_t);
static constexpr size_t BLOCKSIZE = 8 * WORDSIZE;  // Must be a power of 2

static_assert(BLOCKSIZE % WORDSIZE == 0,
              "A block is an integral number of words");

// Constants must match the ones in GenerateAtomicOperations.py
static_assert(JS_GENERATED_ATOMICS_BLOCKSIZE == BLOCKSIZE);
static_assert(JS_GENERATED_ATOMICS_WORDSIZE == WORDSIZE);

static constexpr size_t WORDMASK = WORDSIZE - 1;
static constexpr size_t BLOCKMASK = BLOCKSIZE - 1;

namespace js {
namespace jit {

static bool UnalignedAccessesAreOK() {
#  ifdef DEBUG
  const char* flag = getenv("JS_NO_UNALIGNED_MEMCPY");
  if (flag && *flag == '1') return false;
#  endif
#  if defined(__x86_64__) || defined(__i386__)
  return true;
#  elif defined(__arm__)
  return !ARMFlags::HasAlignmentFault();
#  elif defined(__aarch64__)
  // This is not necessarily true but it's the best guess right now.
  return true;
#  else
#    error "Unsupported platform"
#  endif
}

#  ifndef JS_64BIT
void AtomicCompilerFence() {
  std::atomic_signal_fence(std::memory_order_acq_rel);
}
#  endif

/**
 * Return `true` if all pointers are aligned to `Alignment`.
 */
template <size_t Alignment>
static inline bool CanCopyAligned(const uint8_t* dest, const uint8_t* src,
                                  const uint8_t* lim) {
  static_assert(mozilla::IsPowerOfTwo(Alignment));
  return ((uintptr_t(dest) | uintptr_t(src) | uintptr_t(lim)) &
          (Alignment - 1)) == 0;
}

/**
 * Return `true` if both pointers have the same alignment and can be aligned to
 * `Alignment`.
 */
template <size_t Alignment>
static inline bool CanAlignTo(const uint8_t* dest, const uint8_t* src) {
  static_assert(mozilla::IsPowerOfTwo(Alignment));
  return ((uintptr_t(dest) ^ uintptr_t(src)) & (Alignment - 1)) == 0;
}

/**
 * Copy a datum smaller than `WORDSIZE`. Prevents tearing when `dest` and `src`
 * are both aligned.
 *
 * No tearing is a requirement for integer TypedArrays.
 *
 * https://tc39.es/ecma262/#sec-isnotearconfiguration
 * https://tc39.es/ecma262/#sec-tear-free-aligned-reads
 * https://tc39.es/ecma262/#sec-valid-executions
 */
static MOZ_ALWAYS_INLINE auto AtomicCopyDownNoTearIfAlignedUnsynchronized(
    uint8_t* dest, const uint8_t* src, const uint8_t* srcEnd) {
  MOZ_ASSERT(src <= srcEnd);
  MOZ_ASSERT(size_t(srcEnd - src) < WORDSIZE);

  if (WORDSIZE > 4 && CanCopyAligned<4>(dest, src, srcEnd)) {
    static_assert(WORDSIZE <= 8, "copies 32-bits at most once");

    if (src < srcEnd) {
      AtomicCopy32Unsynchronized(dest, src);
      dest += 4;
      src += 4;
    }
  } else if (CanCopyAligned<2>(dest, src, srcEnd)) {
    while (src < srcEnd) {
      AtomicCopy16Unsynchronized(dest, src);
      dest += 2;
      src += 2;
    }
  } else {
    while (src < srcEnd) {
      AtomicCopy8Unsynchronized(dest++, src++);
    }
  }
  return std::pair{dest, src};
}

void AtomicMemcpyDownUnsynchronized(uint8_t* dest, const uint8_t* src,
                                    size_t nbytes) {
  JS::AutoSuppressGCAnalysis nogc;

  const uint8_t* lim = src + nbytes;

  // Set up bulk copying.  The cases are ordered the way they are on the
  // assumption that if we can achieve aligned copies even with a little
  // preprocessing then that is better than unaligned copying on a platform
  // that supports it.

  if (nbytes >= WORDSIZE) {
    void (*copyBlock)(uint8_t* dest, const uint8_t* src);
    void (*copyWord)(uint8_t* dest, const uint8_t* src);

    if (CanAlignTo<WORDSIZE>(dest, src)) {
      const uint8_t* cutoff = (const uint8_t*)RoundUp(uintptr_t(src), WORDSIZE);
      MOZ_ASSERT(cutoff <= lim);  // because nbytes >= WORDSIZE

      // Copy initial bytes to align to word size.
      std::tie(dest, src) =
          AtomicCopyDownNoTearIfAlignedUnsynchronized(dest, src, cutoff);

      copyBlock = AtomicCopyBlockDownUnsynchronized;
      copyWord = AtomicCopyWordUnsynchronized;
    } else if (UnalignedAccessesAreOK()) {
      copyBlock = AtomicCopyBlockDownUnsynchronized;
      copyWord = AtomicCopyWordUnsynchronized;
    } else {
      copyBlock = AtomicCopyUnalignedBlockDownUnsynchronized;
      copyWord = AtomicCopyUnalignedWordDownUnsynchronized;
    }

    // Bulk copy, first larger blocks and then individual words.

    const uint8_t* blocklim = src + ((lim - src) & ~BLOCKMASK);
    while (src < blocklim) {
      copyBlock(dest, src);
      dest += BLOCKSIZE;
      src += BLOCKSIZE;
    }

    const uint8_t* wordlim = src + ((lim - src) & ~WORDMASK);
    while (src < wordlim) {
      copyWord(dest, src);
      dest += WORDSIZE;
      src += WORDSIZE;
    }
  }

  // Copy any remaining tail.

  AtomicCopyDownNoTearIfAlignedUnsynchronized(dest, src, lim);
}

/**
 * Copy a datum smaller than `WORDSIZE`. Prevents tearing when `dest` and `src`
 * are both aligned.
 *
 * No tearing is a requirement for integer TypedArrays.
 *
 * https://tc39.es/ecma262/#sec-isnotearconfiguration
 * https://tc39.es/ecma262/#sec-tear-free-aligned-reads
 * https://tc39.es/ecma262/#sec-valid-executions
 */
static MOZ_ALWAYS_INLINE auto AtomicCopyUpNoTearIfAlignedUnsynchronized(
    uint8_t* dest, const uint8_t* src, const uint8_t* srcBegin) {
  MOZ_ASSERT(src >= srcBegin);
  MOZ_ASSERT(size_t(src - srcBegin) < WORDSIZE);

  if (WORDSIZE > 4 && CanCopyAligned<4>(dest, src, srcBegin)) {
    static_assert(WORDSIZE <= 8, "copies 32-bits at most once");

    if (src > srcBegin) {
      dest -= 4;
      src -= 4;
      AtomicCopy32Unsynchronized(dest, src);
    }
  } else if (CanCopyAligned<2>(dest, src, srcBegin)) {
    while (src > srcBegin) {
      dest -= 2;
      src -= 2;
      AtomicCopy16Unsynchronized(dest, src);
    }
  } else {
    while (src > srcBegin) {
      AtomicCopy8Unsynchronized(--dest, --src);
    }
  }
  return std::pair{dest, src};
}

void AtomicMemcpyUpUnsynchronized(uint8_t* dest, const uint8_t* src,
                                  size_t nbytes) {
  JS::AutoSuppressGCAnalysis nogc;

  const uint8_t* lim = src;

  src += nbytes;
  dest += nbytes;

  // Set up bulk copying.  The cases are ordered the way they are on the
  // assumption that if we can achieve aligned copies even with a little
  // preprocessing then that is better than unaligned copying on a platform
  // that supports it.

  if (nbytes >= WORDSIZE) {
    void (*copyBlock)(uint8_t* dest, const uint8_t* src);
    void (*copyWord)(uint8_t* dest, const uint8_t* src);

    if (CanAlignTo<WORDSIZE>(dest, src)) {
      const uint8_t* cutoff = (const uint8_t*)(uintptr_t(src) & ~WORDMASK);
      MOZ_ASSERT(cutoff >= lim);  // Because nbytes >= WORDSIZE

      // Copy initial bytes to align to word size.
      std::tie(dest, src) =
          AtomicCopyUpNoTearIfAlignedUnsynchronized(dest, src, cutoff);

      copyBlock = AtomicCopyBlockUpUnsynchronized;
      copyWord = AtomicCopyWordUnsynchronized;
    } else if (UnalignedAccessesAreOK()) {
      copyBlock = AtomicCopyBlockUpUnsynchronized;
      copyWord = AtomicCopyWordUnsynchronized;
    } else {
      copyBlock = AtomicCopyUnalignedBlockUpUnsynchronized;
      copyWord = AtomicCopyUnalignedWordUpUnsynchronized;
    }

    // Bulk copy, first larger blocks and then individual words.

    const uint8_t* blocklim = src - ((src - lim) & ~BLOCKMASK);
    while (src > blocklim) {
      dest -= BLOCKSIZE;
      src -= BLOCKSIZE;
      copyBlock(dest, src);
    }

    const uint8_t* wordlim = src - ((src - lim) & ~WORDMASK);
    while (src > wordlim) {
      dest -= WORDSIZE;
      src -= WORDSIZE;
      copyWord(dest, src);
    }
  }

  // Copy any remaining tail.

  AtomicCopyUpNoTearIfAlignedUnsynchronized(dest, src, lim);
}

}  // namespace jit
}  // namespace js

#endif  // JS_HAVE_GENERATED_ATOMIC_OPS
