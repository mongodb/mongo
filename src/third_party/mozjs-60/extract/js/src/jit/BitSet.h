/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BitSet_h
#define jit_BitSet_h

#include "mozilla/MathAlgorithms.h"

#include "jit/JitAllocPolicy.h"

namespace js {
namespace jit {

// Provides constant time set insertion and removal, and fast linear
// set operations such as intersection, difference, and union.
// N.B. All set operations must be performed on sets with the same number
// of bits.
class BitSet
{
  public:
    static const size_t BitsPerWord = 8 * sizeof(uint32_t);

    static size_t RawLengthForBits(size_t bits) {
        return (bits + BitsPerWord - 1) / BitsPerWord;
    }

  private:
    uint32_t* bits_;
    const unsigned int numBits_;

    static inline uint32_t bitForValue(unsigned int value) {
        return 1l << uint32_t(value % BitsPerWord);
    }

    static inline unsigned int wordForValue(unsigned int value) {
        return value / BitsPerWord;
    }

    inline unsigned int numWords() const {
        return RawLengthForBits(numBits_);
    }

    BitSet(const BitSet&) = delete;
    void operator=(const BitSet&) = delete;

  public:
    class Iterator;

    explicit BitSet(unsigned int numBits) :
        bits_(nullptr),
        numBits_(numBits) {}

    MOZ_MUST_USE bool init(TempAllocator& alloc);

    unsigned int getNumBits() const {
        return numBits_;
    }

    // O(1): Check if this set contains the given value.
    bool contains(unsigned int value) const {
        MOZ_ASSERT(bits_);
        MOZ_ASSERT(value < numBits_);

        return !!(bits_[wordForValue(value)] & bitForValue(value));
    }

    // O(numBits): Check if this set contains any value.
    bool empty() const;

    // O(1): Insert the given value into this set.
    void insert(unsigned int value) {
        MOZ_ASSERT(bits_);
        MOZ_ASSERT(value < numBits_);

        bits_[wordForValue(value)] |= bitForValue(value);
    }

    // O(numBits): Insert every element of the given set into this set.
    void insertAll(const BitSet& other);

    // O(1): Remove the given value from this set.
    void remove(unsigned int value) {
        MOZ_ASSERT(bits_);
        MOZ_ASSERT(value < numBits_);

        bits_[wordForValue(value)] &= ~bitForValue(value);
    }

    // O(numBits): Remove the every element of the given set from this set.
    void removeAll(const BitSet& other);

    // O(numBits): Intersect this set with the given set.
    void intersect(const BitSet& other);

    // O(numBits): Intersect this set with the given set; return whether the
    // intersection caused the set to change.
    bool fixedPointIntersect(const BitSet& other);

    // O(numBits): Does inplace complement of the set.
    void complement();

    // O(numBits): Clear this set.
    void clear();

    uint32_t* raw() const {
        return bits_;
    }
    size_t rawLength() const {
        return numWords();
    }
};

class BitSet::Iterator
{
  private:
    BitSet& set_;
    unsigned index_;
    unsigned word_;
    uint32_t value_;

    void skipEmpty() {
        // Skip words containing only zeros.
        unsigned numWords = set_.numWords();
        const uint32_t* bits = set_.bits_;
        while (value_ == 0) {
            word_++;
            if (word_ == numWords)
                return;

            index_ = word_ * BitSet::BitsPerWord;
            value_ = bits[word_];
        }

        // Be careful: the result of CountTrailingZeroes32 is undefined if the
        // input is 0.
        int numZeros = mozilla::CountTrailingZeroes32(value_);
        index_ += numZeros;
        value_ >>= numZeros;

        MOZ_ASSERT_IF(index_ < set_.numBits_, set_.contains(index_));
    }

  public:
    explicit Iterator(BitSet& set) :
      set_(set),
      index_(0),
      word_(0),
      value_(set.bits_[0])
    {
        skipEmpty();
    }

    inline bool more() const {
        return word_ < set_.numWords();
    }
    explicit operator bool() const {
        return more();
    }

    inline void operator++() {
        MOZ_ASSERT(more());
        MOZ_ASSERT(index_ < set_.numBits_);

        index_++;
        value_ >>= 1;

        skipEmpty();
    }

    unsigned int operator*() {
        MOZ_ASSERT(index_ < set_.numBits_);
        return index_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_BitSet_h */
