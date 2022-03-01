/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_Memory_h
#define util_Memory_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>

static MOZ_ALWAYS_INLINE void* js_memcpy(void* dst_, const void* src_,
                                         size_t len) {
  char* dst = (char*)dst_;
  const char* src = (const char*)src_;
  MOZ_ASSERT_IF(dst >= src, (size_t)(dst - src) >= len);
  MOZ_ASSERT_IF(src >= dst, (size_t)(src - dst) >= len);

  return memcpy(dst, src, len);
}

namespace js {

template <typename T, typename U>
static constexpr U ComputeByteAlignment(T bytes, U alignment) {
  static_assert(std::is_unsigned_v<U>, "alignment amount must be unsigned");

  return (alignment - (bytes % alignment)) % alignment;
}

template <typename T, typename U>
static constexpr T AlignBytes(T bytes, U alignment) {
  static_assert(std::is_unsigned_v<U>, "alignment amount must be unsigned");

  return bytes + ComputeByteAlignment(bytes, alignment);
}

} /* namespace js */

#endif /* util_Memory_h */
