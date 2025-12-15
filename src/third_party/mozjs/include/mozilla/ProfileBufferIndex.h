/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfileBufferIndex_h
#define ProfileBufferIndex_h

#include "mozilla/Attributes.h"

#include <cstddef>
#include <cstdint>

namespace mozilla {

// Generic index into a Profiler buffer, mostly for internal usage.
// Intended to appear infinite (it should effectively never wrap).
// 0 (zero) is reserved as nullptr-like value; it may indicate failure result,
// or it may point at the earliest available block.
using ProfileBufferIndex = uint64_t;

// Externally-opaque class encapsulating a block index, i.e. a
// ProfileBufferIndex that is guaranteed to point at the start of a Profile
// buffer block (until it is destroyed, but then that index cannot be reused and
// functions should gracefully handle expired blocks).
// Users may get these from Profile buffer functions, to later access previous
// blocks; they should avoid converting and operating on their value.
class ProfileBufferBlockIndex {
 public:
  // Default constructor with internal 0 value, for which Profile buffers must
  // guarantee that it is before any valid entries; All public APIs should
  // fail gracefully, doing and/or returning Nothing.
  ProfileBufferBlockIndex() : mBlockIndex(0) {}

  // Implicit conversion from literal `nullptr` to internal 0 value, to allow
  // convenient init/reset/comparison with 0 index.
  MOZ_IMPLICIT ProfileBufferBlockIndex(std::nullptr_t) : mBlockIndex(0) {}

  // Explicit conversion to bool, works in `if` and other tests.
  // Only returns false for default `ProfileBufferBlockIndex{}` value.
  explicit operator bool() const { return mBlockIndex != 0; }

  // Comparison operators. Default `ProfileBufferBlockIndex{}` value is always
  // the lowest.
  [[nodiscard]] bool operator==(const ProfileBufferBlockIndex& aRhs) const {
    return mBlockIndex == aRhs.mBlockIndex;
  }
  [[nodiscard]] bool operator!=(const ProfileBufferBlockIndex& aRhs) const {
    return mBlockIndex != aRhs.mBlockIndex;
  }
  [[nodiscard]] bool operator<(const ProfileBufferBlockIndex& aRhs) const {
    return mBlockIndex < aRhs.mBlockIndex;
  }
  [[nodiscard]] bool operator<=(const ProfileBufferBlockIndex& aRhs) const {
    return mBlockIndex <= aRhs.mBlockIndex;
  }
  [[nodiscard]] bool operator>(const ProfileBufferBlockIndex& aRhs) const {
    return mBlockIndex > aRhs.mBlockIndex;
  }
  [[nodiscard]] bool operator>=(const ProfileBufferBlockIndex& aRhs) const {
    return mBlockIndex >= aRhs.mBlockIndex;
  }

  // Explicit conversion to ProfileBufferIndex, mostly used by internal Profile
  // buffer code.
  [[nodiscard]] ProfileBufferIndex ConvertToProfileBufferIndex() const {
    return mBlockIndex;
  }

  // Explicit creation from ProfileBufferIndex, mostly used by internal
  // Profile buffer code.
  [[nodiscard]] static ProfileBufferBlockIndex CreateFromProfileBufferIndex(
      ProfileBufferIndex aIndex) {
    return ProfileBufferBlockIndex(aIndex);
  }

 private:
  // Private to prevent easy construction from any value. Use
  // `CreateFromProfileBufferIndex()` instead.
  // The main reason for this indirection is to make it harder to create these
  // objects, because only the profiler code should need to do it. Ideally, this
  // class should be used wherever a block index should be stored, but there is
  // so much code that uses `uint64_t` that it would be a big task to change
  // them all. So for now we allow conversions to/from numbers, but it's as ugly
  // as possible to make sure it doesn't get too common; and if one day we want
  // to tackle a global change, it should be easy to find all these locations
  // thanks to the explicit conversion functions.
  explicit ProfileBufferBlockIndex(ProfileBufferIndex aBlockIndex)
      : mBlockIndex(aBlockIndex) {}

  ProfileBufferIndex mBlockIndex;
};

}  // namespace mozilla

#endif  // ProfileBufferIndex_h
