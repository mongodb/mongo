/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_BitSet_h
#define mozilla_BitSet_h

#include "mozilla/Array.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Span.h"

namespace mozilla {

enum MemoryOrdering : uint8_t;
template <typename T, MemoryOrdering Order, typename Enable>
class Atomic;

namespace detail {

template <typename T>
struct UnwrapMaybeAtomic {
  using Type = T;
};
template <typename T, MemoryOrdering Order, typename Enable>
struct UnwrapMaybeAtomic<mozilla::Atomic<T, Order, Enable>> {
  using Type = T;
};

}  // namespace detail

/**
 * An object like std::bitset but which provides access to the underlying
 * storage.
 *
 * The type |StorageType| must be an unsigned integer or a mozilla::Atomic
 * wrapping an unsigned integer. Use of atomic types makes word access atomic,
 * but does not make operations that operate on the whole bitset atomic.
 *
 * The limited API is due to expedience only; feel free to flesh out any
 * std::bitset-like members.
 */
template <size_t N, typename StorageType = size_t>
class BitSet {
 public:
  using Word = typename detail::UnwrapMaybeAtomic<StorageType>::Type;
  static_assert(sizeof(Word) == sizeof(StorageType));
  static_assert(
      std::is_unsigned_v<Word>,
      "StorageType must be an unsigned integral type, or equivalent Atomic");
  static_assert(N != 0);

 private:
  static constexpr size_t kBitsPerWord = 8 * sizeof(Word);
  static constexpr size_t kNumWords = (N + kBitsPerWord - 1) / kBitsPerWord;
  static constexpr size_t kPaddingBits = (kNumWords * kBitsPerWord) - N;
  static constexpr Word kPaddingMask = Word(-1) >> kPaddingBits;

  // The zeroth bit in the bitset is the least significant bit of mStorage[0].
  Array<StorageType, kNumWords> mStorage;

  constexpr void ResetPaddingBits() {
    if constexpr (kPaddingBits != 0) {
      mStorage[kNumWords - 1] &= kPaddingMask;
    }
  }

 public:
  class Reference {
   public:
    Reference(BitSet<N, StorageType>& aBitSet, size_t aPos)
        : mBitSet(aBitSet), mPos(aPos) {}

    Reference& operator=(bool aValue) {
      auto bit = Word(1) << (mPos % kBitsPerWord);
      auto& word = mBitSet.mStorage[mPos / kBitsPerWord];
      if (aValue) {
        word |= bit;
      } else {
        word &= ~bit;
      }
      return *this;
    }

    MOZ_IMPLICIT operator bool() const { return mBitSet.test(mPos); }

   private:
    BitSet<N, StorageType>& mBitSet;
    size_t mPos;
  };

  constexpr BitSet() : mStorage() {}

  BitSet(const BitSet& aOther) { *this = aOther; }

  BitSet& operator=(const BitSet& aOther) {
    for (size_t i = 0; i < std::size(mStorage); i++) {
      mStorage[i] = Word(aOther.mStorage[i]);
    }
    return *this;
  }

  explicit BitSet(Span<StorageType, kNumWords> aStorage) {
    for (size_t i = 0; i < std::size(mStorage); i++) {
      mStorage[i] = Word(aStorage[i]);
    }
  }

  static constexpr size_t size() { return N; }

  constexpr bool test(size_t aPos) const {
    MOZ_ASSERT(aPos < N);
    return mStorage[aPos / kBitsPerWord] & (Word(1) << (aPos % kBitsPerWord));
  }

  constexpr bool IsEmpty() const {
    for (const StorageType& word : mStorage) {
      if (word) {
        return false;
      }
    }
    return true;
  }

  explicit constexpr operator bool() { return !IsEmpty(); }

  constexpr bool operator[](size_t aPos) const { return test(aPos); }

  Reference operator[](size_t aPos) {
    MOZ_ASSERT(aPos < N);
    return {*this, aPos};
  }

  BitSet operator|(const BitSet<N, StorageType>& aOther) {
    BitSet result = *this;
    result |= aOther;
    return result;
  }

  BitSet& operator|=(const BitSet<N, StorageType>& aOther) {
    for (size_t i = 0; i < std::size(mStorage); i++) {
      mStorage[i] |= aOther.mStorage[i];
    }
    return *this;
  }

  BitSet operator~() const {
    BitSet result = *this;
    result.Flip();
    return result;
  }

  BitSet& operator&=(const BitSet<N, StorageType>& aOther) {
    for (size_t i = 0; i < std::size(mStorage); i++) {
      mStorage[i] &= aOther.mStorage[i];
    }
    return *this;
  }

  BitSet operator&(const BitSet<N, StorageType>& aOther) const {
    BitSet result = *this;
    result &= aOther;
    return result;
  }

  bool operator==(const BitSet<N, StorageType>& aOther) const {
    return mStorage == aOther.mStorage;
  }

  size_t Count() const {
    size_t count = 0;

    for (const StorageType& word : mStorage) {
      if constexpr (kBitsPerWord > 32) {
        count += CountPopulation64(word);
      } else {
        count += CountPopulation32(word);
      }
    }

    return count;
  }

  // Set all bits to false.
  void ResetAll() {
    for (StorageType& word : mStorage) {
      word = Word(0);
    }
  }

  // Set all bits to true.
  void SetAll() {
    for (StorageType& word : mStorage) {
      word = ~Word(0);
    }
    ResetPaddingBits();
  }

  void Flip() {
    for (StorageType& word : mStorage) {
      word = ~word;
    }

    ResetPaddingBits();
  }

  // Return the position of the first bit set, or SIZE_MAX if none.
  size_t FindFirst() const { return FindNext(0); }

  // Return the position of the next bit set starting from |aFromPos| inclusive,
  // or SIZE_MAX if none.
  size_t FindNext(size_t aFromPos) const {
    MOZ_ASSERT(aFromPos < N);
    size_t wordIndex = aFromPos / kBitsPerWord;
    size_t bitIndex = aFromPos % kBitsPerWord;

    Word word = mStorage[wordIndex];
    // Mask word containing |aFromPos|.
    word &= (Word(-1) << bitIndex);
    while (word == 0) {
      wordIndex++;
      if (wordIndex == kNumWords) {
        return SIZE_MAX;
      }
      word = mStorage[wordIndex];
    }

    uint_fast8_t pos = CountTrailingZeroes(word);
    return wordIndex * kBitsPerWord + pos;
  }

  size_t FindLast() const { return FindPrev(size() - 1); }

  // Return the position of the previous bit set starting from |aFromPos|
  // inclusive, or SIZE_MAX if none.
  size_t FindPrev(size_t aFromPos) const {
    MOZ_ASSERT(aFromPos < N);
    size_t wordIndex = aFromPos / kBitsPerWord;
    size_t bitIndex = aFromPos % kBitsPerWord;

    Word word = mStorage[wordIndex];
    // Mask word containing |aFromPos|.
    word &= Word(-1) >> (kBitsPerWord - 1 - bitIndex);
    while (word == 0) {
      if (wordIndex == 0) {
        return SIZE_MAX;
      }
      wordIndex--;
      word = mStorage[wordIndex];
    }

    uint_fast8_t pos = FindMostSignificantBit(word);
    return wordIndex * kBitsPerWord + pos;
  }

  Span<StorageType> Storage() { return mStorage; }

  Span<const StorageType> Storage() const { return mStorage; }
};

}  // namespace mozilla

#endif  // mozilla_BitSet_h
