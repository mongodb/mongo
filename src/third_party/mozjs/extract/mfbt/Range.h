/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Range_h
#define mozilla_Range_h

#include "mozilla/RangedPtr.h"
#include "mozilla/Span.h"

#include <stddef.h>
#include <type_traits>

namespace mozilla {

// Range<T> is a tuple containing a pointer and a length.
template <typename T>
class Range {
  template <typename U>
  friend class Range;

  // Reassignment of RangedPtrs is so (subtly) restrictive that we just make
  // Range immutable.
  const RangedPtr<T> mStart;
  const RangedPtr<T> mEnd;

 public:
  Range() : mStart(nullptr, 0), mEnd(nullptr, 0) {}
  Range(T* aPtr, size_t aLength)
      : mStart(aPtr, aPtr, aPtr + aLength),
        mEnd(aPtr + aLength, aPtr, aPtr + aLength) {
    if (!aPtr) {
      MOZ_ASSERT_DEBUG_OR_FUZZING(
          !aLength, "Range does not support nullptr with non-zero length.");
      // ...because merely having a pointer to `nullptr + 1` is undefined
      // behavior. UBSAN catches this as of clang-10.
    }
  }
  Range(const RangedPtr<T>& aStart, const RangedPtr<T>& aEnd)
      : mStart(aStart.get(), aStart.get(), aEnd.get()),
        mEnd(aEnd.get(), aStart.get(), aEnd.get()) {
    // Only accept two RangedPtrs within the same range.
    aStart.checkIdenticalRange(aEnd);
    MOZ_ASSERT_DEBUG_OR_FUZZING(aStart <= aEnd);
  }

  template <typename U, class = std::enable_if_t<
                            std::is_convertible_v<U (*)[], T (*)[]>, int>>
  MOZ_IMPLICIT Range(const Range<U>& aOther)
      : mStart(aOther.mStart), mEnd(aOther.mEnd) {}

  MOZ_IMPLICIT Range(Span<T> aSpan) : Range(aSpan.Elements(), aSpan.Length()) {}

  template <typename U, class = std::enable_if_t<
                            std::is_convertible_v<U (*)[], T (*)[]>, int>>
  MOZ_IMPLICIT Range(const Span<U>& aSpan)
      : Range(aSpan.Elements(), aSpan.Length()) {}

  RangedPtr<T> begin() const { return mStart; }
  RangedPtr<T> end() const { return mEnd; }
  size_t length() const { return mEnd - mStart; }

  T& operator[](size_t aOffset) const { return mStart[aOffset]; }

  explicit operator bool() const { return mStart != nullptr; }

  operator Span<T>() { return Span<T>(mStart.get(), length()); }

  operator Span<const T>() const { return Span<T>(mStart.get(), length()); }
};

template <typename T>
Span(Range<T>&) -> Span<T>;

template <typename T>
Span(const Range<T>&) -> Span<const T>;

}  // namespace mozilla

#endif /* mozilla_Range_h */
