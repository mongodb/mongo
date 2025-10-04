// Copyright 2015, VIXL authors
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

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <inttypes.h>
#include <stdint.h>
}

#include "jstypes.h"

#include "jit/arm64/vixl/Platform-vixl.h"
#include "js/Utility.h"

#ifdef VIXL_NEGATIVE_TESTING
#include <sstream>
#include <stdexcept>
#include <string>
#endif

namespace vixl {

typedef uint8_t byte;

const int KBytes = 1024;
const int MBytes = 1024 * KBytes;

const int kBitsPerByte = 8;

template <int SizeInBits>
struct Unsigned;

template <>
struct Unsigned<32> {
  typedef uint32_t type;
};

template <>
struct Unsigned<64> {
  typedef uint64_t type;
};

}  // namespace vixl

// Detect the host's pointer size.
#if (UINTPTR_MAX == UINT32_MAX)
#define VIXL_HOST_POINTER_32
#elif (UINTPTR_MAX == UINT64_MAX)
#define VIXL_HOST_POINTER_64
#else
#error "Unsupported host pointer size."
#endif

#ifdef VIXL_NEGATIVE_TESTING
#define VIXL_ABORT()                                                         \
  do {                                                                       \
    std::ostringstream oss;                                                  \
    oss << "Aborting in " << __FILE__ << ", line " << __LINE__ << std::endl; \
    throw std::runtime_error(oss.str());                                     \
  } while (false)
#define VIXL_ABORT_WITH_MSG(msg)                                             \
  do {                                                                       \
    std::ostringstream oss;                                                  \
    oss << (msg) << "in " << __FILE__ << ", line " << __LINE__ << std::endl; \
    throw std::runtime_error(oss.str());                                     \
  } while (false)
#define VIXL_CHECK(condition)                                \
  do {                                                       \
    if (!(condition)) {                                      \
      std::ostringstream oss;                                \
      oss << "Assertion failed (" #condition ")\nin ";       \
      oss << __FILE__ << ", line " << __LINE__ << std::endl; \
      throw std::runtime_error(oss.str());                   \
    }                                                        \
  } while (false)
#else
#define VIXL_ABORT()                                         \
  do {                                                       \
    MOZ_CRASH();                                             \
  } while (false)
#define VIXL_ABORT_WITH_MSG(msg)                             \
  do {                                                       \
    MOZ_CRASH(msg);                                          \
  } while (false)
#define VIXL_CHECK(condition)                           \
  do {                                                  \
    if (!(condition)) {                                 \
      MOZ_CRASH();                                      \
    }                                                   \
  } while (false)
#endif
#ifdef DEBUG
#define VIXL_ASSERT(condition) MOZ_ASSERT(condition)
#define VIXL_UNIMPLEMENTED()               \
  do {                                     \
    VIXL_ABORT_WITH_MSG("UNIMPLEMENTED "); \
  } while (false)
#define VIXL_UNREACHABLE()               \
  do {                                   \
    VIXL_ABORT_WITH_MSG("UNREACHABLE "); \
  } while (false)
#else
#define VIXL_ASSERT(condition) ((void)0)
#define VIXL_UNIMPLEMENTED() ((void)0)
#define VIXL_UNREACHABLE() MOZ_CRASH("vixl unreachable")
#endif
// This is not as powerful as template based assertions, but it is simple.
// It assumes that the descriptions are unique. If this starts being a problem,
// we can switch to a different implemention.
#define VIXL_CONCAT(a, b) a##b
#if __cplusplus >= 201103L
#define VIXL_STATIC_ASSERT_LINE(line_unused, condition, message) \
  static_assert(condition, message)
#else
#define VIXL_STATIC_ASSERT_LINE(line, condition, message_unused)            \
  typedef char VIXL_CONCAT(STATIC_ASSERT_LINE_, line)[(condition) ? 1 : -1] \
      __attribute__((unused))
#endif
#define VIXL_STATIC_ASSERT(condition) \
  VIXL_STATIC_ASSERT_LINE(__LINE__, condition, "")
#define VIXL_STATIC_ASSERT_MESSAGE(condition, message) \
  VIXL_STATIC_ASSERT_LINE(__LINE__, condition, message)

#define VIXL_WARNING(message)                                          \
  do {                                                                 \
    printf("WARNING in %s, line %i: %s", __FILE__, __LINE__, message); \
  } while (false)

template <typename T1>
inline void USE(const T1&) {}

template <typename T1, typename T2>
inline void USE(const T1&, const T2&) {}

template <typename T1, typename T2, typename T3>
inline void USE(const T1&, const T2&, const T3&) {}

template <typename T1, typename T2, typename T3, typename T4>
inline void USE(const T1&, const T2&, const T3&, const T4&) {}

#define VIXL_ALIGNMENT_EXCEPTION()                \
  do {                                            \
    VIXL_ABORT_WITH_MSG("ALIGNMENT EXCEPTION\t"); \
  } while (0)

// The clang::fallthrough attribute is used along with the Wimplicit-fallthrough
// argument to annotate intentional fall-through between switch labels.
// For more information please refer to:
// http://clang.llvm.org/docs/AttributeReference.html#fallthrough-clang-fallthrough
#ifndef __has_warning
#define __has_warning(x) 0
#endif

// Fallthrough annotation for Clang and C++11(201103L).
#if __has_warning("-Wimplicit-fallthrough") && __cplusplus >= 201103L
#define VIXL_FALLTHROUGH() [[clang::fallthrough]]
// Fallthrough annotation for GCC >= 7.
#elif __GNUC__ >= 7
#define VIXL_FALLTHROUGH() __attribute__((fallthrough))
#else
#define VIXL_FALLTHROUGH() \
  do {                     \
  } while (0)
#endif

#if __cplusplus >= 201103L
#define VIXL_NO_RETURN [[noreturn]]
#else
#define VIXL_NO_RETURN __attribute__((noreturn))
#endif
#ifdef VIXL_DEBUG
#define VIXL_NO_RETURN_IN_DEBUG_MODE VIXL_NO_RETURN
#else
#define VIXL_NO_RETURN_IN_DEBUG_MODE
#endif

#if __cplusplus >= 201103L
#define VIXL_OVERRIDE override
#else
#define VIXL_OVERRIDE
#endif

#ifdef VIXL_INCLUDE_SIMULATOR_AARCH64
#ifndef VIXL_AARCH64_GENERATE_SIMULATOR_CODE
#define VIXL_AARCH64_GENERATE_SIMULATOR_CODE 1
#endif
#else
#ifndef VIXL_AARCH64_GENERATE_SIMULATOR_CODE
#define VIXL_AARCH64_GENERATE_SIMULATOR_CODE 0
#endif
#if VIXL_AARCH64_GENERATE_SIMULATOR_CODE
#warning "Generating Simulator instructions without Simulator support."
#endif
#endif

// We do not have a simulator for AArch32, although we can pretend we do so that
// tests that require running natively can be skipped.
#ifndef __arm__
#define VIXL_INCLUDE_SIMULATOR_AARCH32
#ifndef VIXL_AARCH32_GENERATE_SIMULATOR_CODE
#define VIXL_AARCH32_GENERATE_SIMULATOR_CODE 1
#endif
#else
#ifndef VIXL_AARCH32_GENERATE_SIMULATOR_CODE
#define VIXL_AARCH32_GENERATE_SIMULATOR_CODE 0
#endif
#endif

// Target Architecture/ISA

// Hack: always include AArch64.
#define VIXL_INCLUDE_TARGET_A64

#ifdef VIXL_INCLUDE_TARGET_A64
#define VIXL_INCLUDE_TARGET_AARCH64
#endif

#if defined(VIXL_INCLUDE_TARGET_A32) && defined(VIXL_INCLUDE_TARGET_T32)
#define VIXL_INCLUDE_TARGET_AARCH32
#elif defined(VIXL_INCLUDE_TARGET_A32)
#define VIXL_INCLUDE_TARGET_A32_ONLY
#else
#define VIXL_INCLUDE_TARGET_T32_ONLY
#endif


#endif  // VIXL_GLOBALS_H
