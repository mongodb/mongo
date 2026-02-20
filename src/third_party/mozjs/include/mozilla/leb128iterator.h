/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// LEB128 utilities that can read/write unsigned LEB128 numbers from/to
// iterators.
//
// LEB128 = Little Endian Base 128, where small numbers take few bytes, but
// large numbers are still allowed, which is ideal when serializing numbers that
// are likely to be small.
// Each byte contains 7 bits from the number, starting at the "little end", the
// top bit is 0 for the last byte, 1 otherwise.
// Numbers 0-127 only take 1 byte. 128-16383 take 2 bytes. Etc.
//
// Iterators only need to provide:
// - `*it` to return a reference to the next byte to be read from or written to.
// - `++it` to advance the iterator after a byte is written.
//
// The caller must always provide sufficient space to write any number, by:
// - pre-allocating a large enough buffer, or
// - allocating more space when `++it` reaches the end and/or `*it` is invoked
//   after the end, or
// - moving the underlying pointer to an appropriate location (e.g., wrapping
//   around a circular buffer).
// The caller must also provide enough bytes to read a full value (i.e., at
// least one byte should have its top bit unset), and a type large enough to
// hold the stored value.
//
// Note: There are insufficient checks for validity! These functions are
// intended to be used together, i.e., the user should only `ReadULEB128()` from
// a sufficiently-large buffer that the same user filled with `WriteULEB128()`.
// Using with externally-sourced data (e.g., DWARF) is *not* recommended.
//
// https://en.wikipedia.org/wiki/LEB128

#ifndef leb128iterator_h
#define leb128iterator_h

#include "mozilla/Assertions.h"
#include "mozilla/Likely.h"

#include <climits>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace mozilla {

// Number of bytes needed to represent `aValue`.
template <typename T>
constexpr uint_fast8_t ULEB128Size(T aValue) {
  static_assert(!std::numeric_limits<T>::is_signed,
                "ULEB128Size only takes unsigned types");
  // We need one output byte per 7 bits of non-zero value. So we just remove
  // 7 least significant bits at a time until the value becomes zero.
  // Note the special case of 0, which still needs 1 output byte; this is done
  // by starting the first loop before we check for 0.
  uint_fast8_t size = 0;
  for (;;) {
    size += 1;
    aValue >>= 7;
    // Expecting small values, so it should be more likely that `aValue == 0`.
    if (MOZ_LIKELY(aValue == 0)) {
      return size;
    }
  }
}

// Maximum number of bytes needed to represent any value of type `T`.
template <typename T>
constexpr uint_fast8_t ULEB128MaxSize() {
  return ULEB128Size<T>(std::numeric_limits<T>::max());
}

// Write `aValue` in LEB128 to `aIterator`.
// The iterator will be moved past the last byte.
template <typename T, typename It>
void WriteULEB128(T aValue, It& aIterator) {
  static_assert(!std::numeric_limits<T>::is_signed,
                "WriteULEB128 only takes unsigned types");
  using IteratorValue = std::remove_reference_t<decltype(*aIterator)>;
  static_assert(sizeof(IteratorValue) == 1,
                "WriteULEB128 expects an iterator to single bytes");
  // 0. Don't test for 0 yet, as we want to output one byte for it.
  for (;;) {
    // 1. Extract the 7 least significant bits.
    const uint_fast8_t byte = aValue & 0x7Fu;
    // 2. Remove them from `aValue`.
    aValue >>= 7;
    // 3. Write the 7 bits, and set the 8th bit if `aValue` is not 0 yet
    // (meaning there will be more bytes after this one.)
    // Expecting small values, so it should be more likely that `aValue == 0`.
    // Note: No absolute need to force-cast to IteratorValue, because we have
    // only changed the bottom 8 bits above. However the compiler could warn
    // about a narrowing conversion from potentially-multibyte uint_fast8_t down
    // to whatever single-byte type `*iterator* expects, so we make it explicit.
    *aIterator = static_cast<IteratorValue>(
        MOZ_LIKELY(aValue == 0) ? byte : (byte | 0x80u));
    // 4. Always advance the iterator to the next byte.
    ++aIterator;
    // 5. We're done if `aValue` is 0.
    // Expecting small values, so it should be more likely that `aValue == 0`.
    if (MOZ_LIKELY(aValue == 0)) {
      return;
    }
  }
}

// Read an LEB128 value from `aIterator`.
// The iterator will be moved past the last byte.
template <typename T, typename It>
T ReadULEB128(It& aIterator) {
  static_assert(!std::numeric_limits<T>::is_signed,
                "ReadULEB128 must return an unsigned type");
  using IteratorValue = std::remove_reference_t<decltype(*aIterator)>;
  static_assert(sizeof(IteratorValue) == 1,
                "ReadULEB128 expects an iterator to single bytes");
  // Incoming bits will be added to `result`...
  T result = 0;
  // ... starting with the least significant bits.
  uint_fast8_t shift = 0;
  for (;;) {
    // 1. Read one byte from the iterator.
    // `static_cast` just in case IteratorValue is not implicitly convertible to
    // uint_fast8_t. It wouldn't matter if the sign was extended, we're only
    // dealing with the bottom 8 bits below.
    const uint_fast8_t byte = static_cast<uint_fast8_t>(*aIterator);
    // 2. Always advance the iterator.
    ++aIterator;
    // 3. Extract the 7 bits of value, and shift them in place into `result`.
    result |= static_cast<T>(byte & 0x7fu) << shift;
    // 4. If the 8th bit is *not* set, this was the last byte.
    // Expecting small values, so it should be more likely that the bit is off.
    if (MOZ_LIKELY((byte & 0x80u) == 0)) {
      return result;
    }
    // There are more bytes to read.
    // 5. Next byte will contain more significant bits above the past 7.
    shift += 7;
    // Safety check that we're not going to shift by >= than the type size,
    // which is Undefined Behavior in C++.
    MOZ_ASSERT(shift < CHAR_BIT * sizeof(T));
  }
}

// constexpr ULEB128 reader class.
// Mostly useful when dealing with non-trivial byte feeds.
template <typename T>
class ULEB128Reader {
  static_assert(!std::numeric_limits<T>::is_signed,
                "ULEB128Reader must handle an unsigned type");

 public:
  constexpr ULEB128Reader() = default;

  // Don't allow copy/assignment, it doesn't make sense for a stateful parser.
  constexpr ULEB128Reader(const ULEB128Reader&) = delete;
  constexpr ULEB128Reader& operator=(const ULEB128Reader&) = delete;

  // Feed a byte into the parser.
  // Returns true if this was the last byte.
  [[nodiscard]] constexpr bool FeedByteIsComplete(unsigned aByte) {
    MOZ_ASSERT(!IsComplete());
    // Extract the 7 bits of value, and shift them in place into the value.
    mValue |= static_cast<T>(aByte & 0x7fu) << mShift;
    // If the 8th bit is *not* set, this was the last byte.
    // Expecting small values, so it should be more likely that the bit is off.
    if (MOZ_LIKELY((aByte & 0x80u) == 0)) {
      mShift = mCompleteShift;
      return true;
    }
    // There are more bytes to read.
    // Next byte will contain more significant bits above the past 7.
    mShift += 7;
    // Safety check that we're not going to shift by >= than the type size,
    // which is Undefined Behavior in C++.
    MOZ_ASSERT(mShift < CHAR_BIT * sizeof(T));
    return false;
  }

  constexpr void Reset() {
    mValue = 0;
    mShift = 0;
  }

  [[nodiscard]] constexpr bool IsComplete() const {
    return mShift == mCompleteShift;
  }

  [[nodiscard]] constexpr T Value() const {
    MOZ_ASSERT(IsComplete());
    return mValue;
  }

 private:
  // Special value of `mShift` indicating that parsing is complete.
  constexpr static unsigned mCompleteShift = 0x10000u;

  T mValue = 0;
  unsigned mShift = 0;
};

}  // namespace mozilla

#endif  // leb128iterator_h
