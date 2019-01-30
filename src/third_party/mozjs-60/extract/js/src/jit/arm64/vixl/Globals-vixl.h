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

#ifndef VIXL_GLOBALS_H
#define VIXL_GLOBALS_H

// Get standard C99 macros for integer types.
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "mozilla/Assertions.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "js-config.h"

#include "jit/arm64/vixl/Platform-vixl.h"
#include "js/Utility.h"


typedef uint8_t byte;

// Type for half-precision (16 bit) floating point numbers.
typedef uint16_t float16;

const int KBytes = 1024;
const int MBytes = 1024 * KBytes;

#define VIXL_ABORT() \
    do { printf("in %s, line %i", __FILE__, __LINE__); abort(); } while (false)
#ifdef DEBUG
  #define VIXL_ASSERT(condition) MOZ_ASSERT(condition)
  #define VIXL_CHECK(condition) VIXL_ASSERT(condition)
  #define VIXL_UNIMPLEMENTED() \
    do { fprintf(stderr, "UNIMPLEMENTED\t"); VIXL_ABORT(); } while (false)
  #define VIXL_UNREACHABLE() \
    do { fprintf(stderr, "UNREACHABLE\t"); VIXL_ABORT(); } while (false)
#else
  #define VIXL_ASSERT(condition) ((void) 0)
  #define VIXL_CHECK(condition) ((void) 0)
  #define VIXL_UNIMPLEMENTED() ((void) 0)
  #define VIXL_UNREACHABLE() MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE()
#endif
// This is not as powerful as template based assertions, but it is simple.
// It assumes that the descriptions are unique. If this starts being a problem,
// we can switch to a different implemention.
#define VIXL_CONCAT(a, b) a##b
#define VIXL_STATIC_ASSERT_LINE(line, condition) \
  typedef char VIXL_CONCAT(STATIC_ASSERT_LINE_, line)[(condition) ? 1 : -1] \
  __attribute__((unused))
#define VIXL_STATIC_ASSERT(condition) \
    VIXL_STATIC_ASSERT_LINE(__LINE__, condition)

template <typename T1>
inline void USE(T1) {}

template <typename T1, typename T2>
inline void USE(T1, T2) {}

template <typename T1, typename T2, typename T3>
inline void USE(T1, T2, T3) {}

template <typename T1, typename T2, typename T3, typename T4>
inline void USE(T1, T2, T3, T4) {}

#define VIXL_ALIGNMENT_EXCEPTION() \
    do { fprintf(stderr, "ALIGNMENT EXCEPTION\t"); VIXL_ABORT(); } while (0)

// The clang::fallthrough attribute is used along with the Wimplicit-fallthrough
// argument to annotate intentional fall-through between switch labels.
// For more information please refer to:
// http://clang.llvm.org/docs/AttributeReference.html#fallthrough-clang-fallthrough
#ifndef __has_warning
  #define __has_warning(x)  0
#endif

// Note: This option is only available for Clang. And will only be enabled for
// C++11(201103L).
#if __has_warning("-Wimplicit-fallthrough") && __cplusplus >= 201103L
  #define VIXL_FALLTHROUGH() [[clang::fallthrough]] //NOLINT
#else
  #define VIXL_FALLTHROUGH() do {} while (0)
#endif

#endif  // VIXL_GLOBALS_H
