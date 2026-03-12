/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_SIMD_h
#define mozilla_SIMD_h

#include "mozilla/Types.h"

namespace mozilla {
// A collection of SIMD-implemented algorithms. Some of these exist in the CRT.
// However, the quality of the C runtime implementation varies wildly across
// platforms, so these should at least ensure consistency.
//
// NOTE: these are currently only implemented with hand-written SIMD for x86
// and AMD64 platforms, and fallback to the the C runtime or naive loops on
// other architectures. Please consider this before switching an already
// optimized loop to these helpers.
class SIMD {
 public:
  // NOTE: for memchr we have a goofy void* signature just to be an easy drop
  // in replacement for the CRT version. We also give memchr8 which is just a
  // typed version of memchr.
  static const void* memchr(const void* ptr, int value, size_t num) {
    return memchr8(reinterpret_cast<const char*>(ptr), static_cast<char>(value),
                   num);
  }

  // Search through `ptr[0..length]` for the first occurrence of `value` and
  // return the pointer to it, or nullptr if it cannot be found.
  static MFBT_API const char* memchr8(const char* ptr, char value,
                                      size_t length);

  // This function just restricts our execution to the SSE2 path
  static MFBT_API const char* memchr8SSE2(const char* ptr, char value,
                                          size_t length);

  // This function just restricts our execution to the AVX2 path
  static MFBT_API const char* memchr8AVX2(const char* ptr, char value,
                                          size_t length);

  // Search through `ptr[0..length]` for the first occurrence of `value` and
  // return the pointer to it, or nullptr if it cannot be found.
  static MFBT_API const char16_t* memchr16(const char16_t* ptr, char16_t value,
                                           size_t length);

  // This function just restricts our execution to the SSE2 path
  static MFBT_API const char16_t* memchr16SSE2(const char16_t* ptr,
                                               char16_t value, size_t length);

  // This function just restricts our execution to the AVX2 path
  static MFBT_API const char16_t* memchr16AVX2(const char16_t* ptr,
                                               char16_t value, size_t length);

  // Search through `ptr[0..length]` for the first occurrence of `value` and
  // return the pointer to it, or nullptr if it cannot be found.
  static MFBT_API const uint32_t* memchr32(const uint32_t* ptr, uint32_t value,
                                           size_t length);

  // This function just restricts our execution to the AVX2 path
  static MFBT_API const uint32_t* memchr32AVX2(const uint32_t* ptr,
                                               uint32_t value, size_t length);

  // Search through `ptr[0..length]` for the first occurrence of `value` and
  // return the pointer to it, or nullptr if it cannot be found.
  static MFBT_API const uint64_t* memchr64(const uint64_t* ptr, uint64_t value,
                                           size_t length);

  // This function just restricts our execution to the AVX2 path
  static MFBT_API const uint64_t* memchr64AVX2(const uint64_t* ptr,
                                               uint64_t value, size_t length);

  // Search through `ptr[0..length]` for the first occurrence of `v1` which is
  // immediately followed by `v2` and return the pointer to the occurrence of
  // `v1`.
  static MFBT_API const char* memchr2x8(const char* ptr, char v1, char v2,
                                        size_t length);

  // Search through `ptr[0..length]` for the first occurrence of `v1` which is
  // immediately followed by `v2` and return the pointer to the occurrence of
  // `v1`.
  static MFBT_API const char16_t* memchr2x16(const char16_t* ptr, char16_t v1,
                                             char16_t v2, size_t length);
};

}  // namespace mozilla

#endif  // mozilla_SIMD_h
