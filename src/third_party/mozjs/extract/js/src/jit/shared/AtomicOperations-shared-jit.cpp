/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AtomicOperations.h"

#if defined(__arm__)
#  include "jit/arm/Architecture-arm.h"
#endif

#ifdef JS_HAVE_GENERATED_ATOMIC_OPS

#  include <atomic>

#  include "js/GCAPI.h"

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
  return !HasAlignmentFault();
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

    if (((uintptr_t(dest) ^ uintptr_t(src)) & WORDMASK) == 0) {
      const uint8_t* cutoff = (const uint8_t*)RoundUp(uintptr_t(src), WORDSIZE);
      MOZ_ASSERT(cutoff <= lim);  // because nbytes >= WORDSIZE
      while (src < cutoff) {
        AtomicCopyByteUnsynchronized(dest++, src++);
      }
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

  // Byte copy any remaining tail.

  while (src < lim) {
    AtomicCopyByteUnsynchronized(dest++, src++);
  }
}

void AtomicMemcpyUpUnsynchronized(uint8_t* dest, const uint8_t* src,
                                  size_t nbytes) {
  JS::AutoSuppressGCAnalysis nogc;

  const uint8_t* lim = src;

  src += nbytes;
  dest += nbytes;

  if (nbytes >= WORDSIZE) {
    void (*copyBlock)(uint8_t* dest, const uint8_t* src);
    void (*copyWord)(uint8_t* dest, const uint8_t* src);

    if (((uintptr_t(dest) ^ uintptr_t(src)) & WORDMASK) == 0) {
      const uint8_t* cutoff = (const uint8_t*)(uintptr_t(src) & ~WORDMASK);
      MOZ_ASSERT(cutoff >= lim);  // Because nbytes >= WORDSIZE
      while (src > cutoff) {
        AtomicCopyByteUnsynchronized(--dest, --src);
      }
      copyBlock = AtomicCopyBlockUpUnsynchronized;
      copyWord = AtomicCopyWordUnsynchronized;
    } else if (UnalignedAccessesAreOK()) {
      copyBlock = AtomicCopyBlockUpUnsynchronized;
      copyWord = AtomicCopyWordUnsynchronized;
    } else {
      copyBlock = AtomicCopyUnalignedBlockUpUnsynchronized;
      copyWord = AtomicCopyUnalignedWordUpUnsynchronized;
    }

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

  while (src > lim) {
    AtomicCopyByteUnsynchronized(--dest, --src);
  }
}

}  // namespace jit
}  // namespace js

#endif  // JS_HAVE_GENERATED_ATOMIC_OPS
