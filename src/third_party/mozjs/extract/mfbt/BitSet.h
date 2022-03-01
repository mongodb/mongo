/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_BitSet_h
#define mozilla_BitSet_h

#include "mozilla/Array.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Span.h"

namespace mozilla {

/**
 * An object like std::bitset but which provides access to the underlying
 * storage.
 *
 * The limited API is due to expedience only; feel free to flesh out any
 * std::bitset-like members.
 */
template <size_t N, typename Word = size_t>
class BitSet {
  static_assert(std::is_unsigned_v<Word>,
                "The Word type must be an unsigned integral type");

 private:
  static constexpr size_t kBitsPerWord = 8 * sizeof(Word);
  static constexpr size_t kNumWords = (N + kBitsPerWord - 1) / kBitsPerWord;

  // The zeroth bit in the bitset is the least significant bit of mStorage[0].
  Array<Word, kNumWords> mStorage;

 public:
  class Reference {
   public:
    Reference(BitSet<N, Word>& aBitSet, size_t aPos)
        : mBitSet(aBitSet), mPos(aPos) {}

    Reference& operator=(bool aValue) {
      auto bit = Word(1) << (mPos % kBitsPerWord);
      auto& word = mBitSet.mStorage[mPos / kBitsPerWord];
      word = (word & ~bit) | (aValue ? bit : 0);
      return *this;
    }

    MOZ_IMPLICIT operator bool() const { return mBitSet.Test(mPos); }

   private:
    BitSet<N, Word>& mBitSet;
    size_t mPos;
  };

  BitSet() { ResetAll(); }

  BitSet(const BitSet& aOther) { *this = aOther; }

  BitSet& operator=(const BitSet& aOther) {
    PodCopy(mStorage.begin(), aOther.mStorage.begin(), kNumWords);
    return *this;
  }

  explicit BitSet(Span<Word, kNumWords> aStorage) {
    PodCopy(mStorage.begin(), aStorage.Elements(), kNumWords);
  }

  constexpr size_t Size() const { return N; }

  constexpr bool Test(size_t aPos) const {
    MOZ_ASSERT(aPos < N);
    return mStorage[aPos / kBitsPerWord] & (Word(1) << (aPos % kBitsPerWord));
  }

  constexpr bool operator[](size_t aPos) const { return Test(aPos); }

  Reference operator[](size_t aPos) {
    MOZ_ASSERT(aPos < N);
    return {*this, aPos};
  }

  BitSet operator|(const BitSet<N, Word>& aOther) {
    BitSet result = *this;
    result |= aOther;
    return result;
  }

  BitSet& operator|=(const BitSet<N, Word>& aOther) {
    for (size_t i = 0; i < ArrayLength(mStorage); i++) {
      mStorage[i] |= aOther.mStorage[i];
    }
    return *this;
  }

  // Set all bits to false.
  void ResetAll() { PodArrayZero(mStorage); }

  // Set all bits to true.
  void SetAll() {
    memset(mStorage.begin(), 0xff, kNumWords * sizeof(Word));
    constexpr size_t paddingBits = (kNumWords * kBitsPerWord) - N;
    constexpr Word paddingMask = Word(-1) >> paddingBits;
    if constexpr (paddingBits != 0) {
      mStorage[kNumWords - 1] &= paddingMask;
    }
  }

  Span<Word> Storage() { return mStorage; }

  Span<const Word> Storage() const { return mStorage; }
};

}  // namespace mozilla

#endif  // mozilla_BitSet_h
