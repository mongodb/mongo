/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * A bit array is an array of bits represented by an array of words (size_t).
 */

#ifndef util_BitArray_h
#define util_BitArray_h

#include "mozilla/Assertions.h"

#include <limits.h>
#include <stddef.h>

namespace js {

static const size_t BitArrayElementBits = sizeof(size_t) * CHAR_BIT;

static inline unsigned NumWordsForBitArrayOfLength(size_t length) {
  return (length + (BitArrayElementBits - 1)) / BitArrayElementBits;
}

static inline unsigned BitArrayIndexToWordIndex(size_t length,
                                                size_t bitIndex) {
  unsigned wordIndex = bitIndex / BitArrayElementBits;
  MOZ_ASSERT(wordIndex < length);
  return wordIndex;
}

static inline size_t BitArrayIndexToWordMask(size_t i) {
  return size_t(1) << (i % BitArrayElementBits);
}

static inline bool IsBitArrayElementSet(const size_t* array, size_t length,
                                        size_t i) {
  return array[BitArrayIndexToWordIndex(length, i)] &
         BitArrayIndexToWordMask(i);
}

static inline bool IsAnyBitArrayElementSet(const size_t* array, size_t length) {
  unsigned numWords = NumWordsForBitArrayOfLength(length);
  for (unsigned i = 0; i < numWords; ++i) {
    if (array[i]) {
      return true;
    }
  }
  return false;
}

static inline void SetBitArrayElement(size_t* array, size_t length, size_t i) {
  array[BitArrayIndexToWordIndex(length, i)] |= BitArrayIndexToWordMask(i);
}

static inline void ClearBitArrayElement(size_t* array, size_t length,
                                        size_t i) {
  array[BitArrayIndexToWordIndex(length, i)] &= ~BitArrayIndexToWordMask(i);
}

static inline void ClearAllBitArrayElements(size_t* array, size_t length) {
  for (unsigned i = 0; i < length; ++i) {
    array[i] = 0;
  }
}

} /* namespace js */

#endif /* util_BitArray_h */
