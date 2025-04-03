/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_BitArray_h
#define ds_BitArray_h

#include "mozilla/MathAlgorithms.h"
#include "mozilla/TemplateLib.h"

#include <limits.h>
#include <string.h>

#include "jstypes.h"

namespace js {

namespace detail {

template <typename WordT>
inline uint_fast8_t CountTrailingZeroes(WordT word);

template <>
inline uint_fast8_t CountTrailingZeroes(uint32_t word) {
  return mozilla::CountTrailingZeroes32(word);
}

template <>
inline uint_fast8_t CountTrailingZeroes(uint64_t word) {
  return mozilla::CountTrailingZeroes64(word);
}

}  // namespace detail

template <size_t nbits>
class BitArray {
 public:
  // Use a 32 bit word to make it easier to access a BitArray from JIT code.
  using WordT = uint32_t;

  static const size_t bitsPerElement = sizeof(WordT) * CHAR_BIT;
  static const size_t numSlots =
      nbits / bitsPerElement + (nbits % bitsPerElement == 0 ? 0 : 1);

 private:
  static const size_t paddingBits = (numSlots * bitsPerElement) - nbits;
  static_assert(paddingBits < bitsPerElement,
                "More padding bits than expected.");
  static const WordT paddingMask = WordT(-1) >> paddingBits;

  WordT map[numSlots];

 public:
  constexpr BitArray() : map(){};

  void clear(bool value) {
    memset(map, value ? 0xFF : 0, sizeof(map));
    if (value) {
      map[numSlots - 1] &= paddingMask;
    }
  }

  inline bool get(size_t offset) const {
    size_t index;
    WordT mask;
    getIndexAndMask(offset, &index, &mask);
    MOZ_ASSERT(index < nbits);
    return map[index] & mask;
  }

  void set(size_t offset) {
    size_t index;
    WordT mask;
    getIndexAndMask(offset, &index, &mask);
    map[index] |= mask;
  }

  void unset(size_t offset) {
    size_t index;
    WordT mask;
    getIndexAndMask(offset, &index, &mask);
    map[index] &= ~mask;
  }

  bool isAllClear() const {
    for (size_t i = 0; i < numSlots; i++) {
      if (map[i]) {
        return false;
      }
    }
    return true;
  }

  // For iterating over the set bits in the bit array, get a word at a time.
  WordT getWord(size_t elementIndex) const {
    MOZ_ASSERT(elementIndex < nbits);
    return map[elementIndex];
  }

  static void getIndexAndMask(size_t offset, size_t* indexp, WordT* maskp) {
    MOZ_ASSERT(offset < nbits);
    static_assert(bitsPerElement == 32, "unexpected bitsPerElement value");
    *indexp = offset / bitsPerElement;
    *maskp = WordT(1) << (offset % bitsPerElement);
  }

  static size_t offsetOfMap() { return offsetof(BitArray<nbits>, map); }
};

} /* namespace js */

#endif /* ds_BitArray_h */
