// Copyright 2015, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


#ifndef VIXL_COMPILER_INTRINSICS_H
#define VIXL_COMPILER_INTRINSICS_H

#include "mozilla/MathAlgorithms.h"

#include "jit/arm64/vixl/Globals-vixl.h"

namespace vixl {

// Helper to check whether the version of GCC used is greater than the specified
// requirement.
#define MAJOR 1000000
#define MINOR 1000
#if defined(__GNUC__) && defined(__GNUC_MINOR__) && defined(__GNUC_PATCHLEVEL__)
#define GCC_VERSION_OR_NEWER(major, minor, patchlevel)                         \
    ((__GNUC__ * MAJOR + __GNUC_MINOR__ * MINOR + __GNUC_PATCHLEVEL__) >=      \
     ((major) * MAJOR + (minor) * MINOR + (patchlevel)))
#elif defined(__GNUC__) && defined(__GNUC_MINOR__)
#define GCC_VERSION_OR_NEWER(major, minor, patchlevel)                         \
    ((__GNUC__ * MAJOR + __GNUC_MINOR__ * MINOR) >=                            \
     ((major) * MAJOR + (minor) * MINOR + (patchlevel)))
#else
#define GCC_VERSION_OR_NEWER(major, minor, patchlevel) 0
#endif


#if defined(__clang__) && !defined(VIXL_NO_COMPILER_BUILTINS)

#define COMPILER_HAS_BUILTIN_CLRSB    (__has_builtin(__builtin_clrsb))
#define COMPILER_HAS_BUILTIN_CLZ      (__has_builtin(__builtin_clz))
#define COMPILER_HAS_BUILTIN_CTZ      (__has_builtin(__builtin_ctz))
#define COMPILER_HAS_BUILTIN_FFS      (__has_builtin(__builtin_ffs))
#define COMPILER_HAS_BUILTIN_POPCOUNT (__has_builtin(__builtin_popcount))

#elif defined(__GNUC__) && !defined(VIXL_NO_COMPILER_BUILTINS)
// The documentation for these builtins is available at:
// https://gcc.gnu.org/onlinedocs/gcc-$MAJOR.$MINOR.$PATCHLEVEL/gcc//Other-Builtins.html

# define COMPILER_HAS_BUILTIN_CLRSB    (GCC_VERSION_OR_NEWER(4, 7, 0))
# define COMPILER_HAS_BUILTIN_CLZ      (GCC_VERSION_OR_NEWER(3, 4, 0))
# define COMPILER_HAS_BUILTIN_CTZ      (GCC_VERSION_OR_NEWER(3, 4, 0))
# define COMPILER_HAS_BUILTIN_FFS      (GCC_VERSION_OR_NEWER(3, 4, 0))
# define COMPILER_HAS_BUILTIN_POPCOUNT (GCC_VERSION_OR_NEWER(3, 4, 0))

#else
// One can define VIXL_NO_COMPILER_BUILTINS to force using the manually
// implemented C++ methods.

#define COMPILER_HAS_BUILTIN_BSWAP    false
#define COMPILER_HAS_BUILTIN_CLRSB    false
#define COMPILER_HAS_BUILTIN_CLZ      false
#define COMPILER_HAS_BUILTIN_CTZ      false
#define COMPILER_HAS_BUILTIN_FFS      false
#define COMPILER_HAS_BUILTIN_POPCOUNT false

#endif


template<typename V>
inline bool IsPowerOf2(V value) {
  return (value != 0) && ((value & (value - 1)) == 0);
}


// Implementation of intrinsics functions.
// TODO: The implementations could be improved for sizes different from 32bit
// and 64bit: we could mask the values and call the appropriate builtin.


template<typename V>
inline int CountLeadingZeros(V value, int width = (sizeof(V) * 8)) {
#if COMPILER_HAS_BUILTIN_CLZ
  if (width == 32) {
    return (value == 0) ? 32 : __builtin_clz(static_cast<unsigned>(value));
  } else if (width == 64) {
    return (value == 0) ? 64 : __builtin_clzll(value);
  }
  MOZ_CRASH("Unhandled width.");
#else
  if (width == 32) {
    return mozilla::CountLeadingZeroes32(value);
  } else if (width == 64) {
    return mozilla::CountLeadingZeroes64(value);
  }
  MOZ_CRASH("Unhandled width.");
#endif
}


template<typename V>
inline int CountLeadingSignBits(V value, int width = (sizeof(V) * 8)) {
#if COMPILER_HAS_BUILTIN_CLRSB
  if (width == 32) {
    return __builtin_clrsb(value);
  } else if (width == 64) {
    return __builtin_clrsbll(value);
  }
  MOZ_CRASH("Unhandled width.");
#else
  VIXL_ASSERT(IsPowerOf2(width) && (width <= 64));
  if (value >= 0) {
    return CountLeadingZeros(value, width) - 1;
  } else {
    return CountLeadingZeros(~value, width) - 1;
  }
#endif
}


template<typename V>
inline int CountSetBits(V value, int width = (sizeof(V) * 8)) {
#if COMPILER_HAS_BUILTIN_POPCOUNT
  if (width == 32) {
    return __builtin_popcount(static_cast<unsigned>(value));
  } else if (width == 64) {
    return __builtin_popcountll(value);
  }
  MOZ_CRASH("Unhandled width.");
#else
  if (width == 32) {
    return mozilla::CountPopulation32(value);
  } else if (width == 64) {
    return mozilla::CountPopulation64(value);
  }
  MOZ_CRASH("Unhandled width.");
#endif
}


template<typename V>
inline int CountTrailingZeros(V value, int width = (sizeof(V) * 8)) {
#if COMPILER_HAS_BUILTIN_CTZ
  if (width == 32) {
    return (value == 0) ? 32 : __builtin_ctz(static_cast<unsigned>(value));
  } else if (width == 64) {
    return (value == 0) ? 64 : __builtin_ctzll(value);
  }
  MOZ_CRASH("Unhandled width.");
#else
  if (width == 32) {
    return mozilla::CountTrailingZeroes32(value);
  } else if (width == 64) {
    return mozilla::CountTrailingZeroes64(value);
  }
  MOZ_CRASH("Unhandled width.");
#endif
}

}  // namespace vixl

#endif  // VIXL_COMPILER_INTRINSICS_H

