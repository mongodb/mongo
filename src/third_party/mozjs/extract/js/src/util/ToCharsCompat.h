/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_ToCharsCompat_h
#define util_ToCharsCompat_h

// MONGODB MODIFICATION: std::to_chars compatibility for macOS < 10.15.
//
// Apple's libc++ marks std::to_chars as unavailable when the deployment target
// is below macOS 10.15. On older backport branches (e.g. v7.0) that still
// target earlier macOS versions, this causes compile failures. This header
// provides a portable fallback that is selected at compile time.
//
// The fallback uses the same backfill-from-end algorithm that SpiderMonkey
// used prior to adopting std::to_chars (see the old Int32ToCStringWithBase
// in jsnum.cpp before ESR 140).

#if defined(__APPLE__) && defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && \
    __MAC_OS_X_VERSION_MIN_REQUIRED < 101500

#include "mozilla/Assertions.h"

#include <limits>
#include <type_traits>

namespace js::detail {

struct ToCharsResult {
  char* ptr;
};

// Backfill-from-end integer-to-string conversion, matching the algorithm
// SpiderMonkey used before std::to_chars was adopted. Writes digits from
// the end of the buffer backwards, then returns a pointer past the last
// written character at the front.
template <typename T>
ToCharsResult portable_to_chars(char* first, char* last, T value, int base) {
  static_assert(std::is_integral_v<T>, "integral types only");
  MOZ_ASSERT(base >= 2 && base <= 36);
  MOZ_ASSERT(first < last);

  using Unsigned = std::make_unsigned_t<T>;
  bool negative = false;
  Unsigned u;
  if constexpr (std::is_signed_v<T>) {
    if (value < 0) {
      negative = true;
      u = static_cast<Unsigned>(~static_cast<Unsigned>(value) + 1u);
    } else {
      u = static_cast<Unsigned>(value);
    }
  } else {
    u = value;
  }

  // Write digits backwards from the end of the buffer.
  char* end = last;
  char* cp = end;
  do {
    Unsigned newu = u / static_cast<Unsigned>(base);
    *--cp = "0123456789abcdefghijklmnopqrstuvwxyz"
        [static_cast<unsigned>(u - newu * static_cast<Unsigned>(base))];
    u = newu;
  } while (u != 0);

  if (negative) {
    *--cp = '-';
  }

  // Move the result to the front of the buffer.
  size_t len = end - cp;
  if (cp != first) {
    for (size_t i = 0; i < len; i++) {
      first[i] = cp[i];
    }
  }

  return {first + len};
}

}  // namespace js::detail

#define MONGO_MOZJS_TO_CHARS(first, last, value, base) \
  js::detail::portable_to_chars(first, last, value, base)

#else  // macOS >= 10.15 or non-Apple platforms: use std::to_chars directly.

#include "mozilla/Assertions.h"

#include <charconv>

namespace js::detail {

template <typename T>
inline std::to_chars_result checked_to_chars(char* first, char* last, T value,
                                             int base) {
  auto r = std::to_chars(first, last, value, base);
  MOZ_ASSERT(r.ec == std::errc());
  return r;
}

}  // namespace js::detail

#define MONGO_MOZJS_TO_CHARS(first, last, value, base) \
  js::detail::checked_to_chars(first, last, value, base)

#endif

#endif /* util_ToCharsCompat_h */
