/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_SparseBitSet_h
#define jit_SparseBitSet_h

#include "mozilla/Assertions.h"
#include "mozilla/MathAlgorithms.h"

#include <stddef.h>
#include <stdint.h>

#include "ds/InlineTable.h"

namespace js::jit {

// jit::SparseBitSet is very similar to jit::BitSet, but optimized for sets that
// can be very large but are often very sparse.
//
// It uses an InlineMap mapping from word-index to 32-bit words storing bits.
//
// For example, consider the following bitmap of 192 bits:
//
//   0xff001100 0x00001000  0x00000000 0x00000000 0x00000000 0x11110000
//   0^         1^          2^         3^         4^         5^
//
// Words 2-4 don't have any bits set, so a SparseBitSet only stores the other
// words:
//
//   0 => 0xff001100
//   1 => 0x00001000
//   5 => 0x11110000
//
// SparseBitSet ensures words in the map are never 0.
template <typename AllocPolicy>
class SparseBitSet {
  // Note: use uint32_t (instead of uintptr_t or uint64_t) to not waste space in
  // InlineMap's array of inline entries. It uses a struct for each key/value
  // pair.
  using WordType = uint32_t;
  static constexpr size_t BitsPerWord = 8 * sizeof(WordType);

  // Note: 8 inline entries is sufficient for the majority of bit sets.
  // Compiling a large PhotoShop Wasm module with Ion, 94.5% of SparseBitSets
  // had <= 8 map entries. For OpenOffice this was more than 98.5%.
  static constexpr size_t NumEntries = 8;
  using Map = InlineMap<uint32_t, WordType, NumEntries, DefaultHasher<uint32_t>,
                        AllocPolicy>;
  using Range = typename Map::Range;
  Map map_;

  static_assert(mozilla::IsPowerOfTwo(BitsPerWord),
                "Must be power-of-two for fast division/modulo");
  static_assert((sizeof(uint32_t) + sizeof(WordType)) * NumEntries ==
                    Map::SizeOfInlineEntries,
                "Array of inline entries must not have unused padding bytes");

  static WordType bitMask(size_t bit) {
    return WordType(1) << (bit % BitsPerWord);
  }

 public:
  class Iterator;

  bool contains(size_t bit) {
    uint32_t word = bit / BitsPerWord;
    if (auto p = map_.lookup(word)) {
      return p->value() & bitMask(bit);
    }
    return false;
  }
  void remove(size_t bit) {
    uint32_t word = bit / BitsPerWord;
    if (auto p = map_.lookup(word)) {
      WordType value = p->value() & ~bitMask(bit);
      if (value != 0) {
        p->value() = value;
      } else {
        // The iterator and empty() method rely on the map not containing
        // entries without any bits set.
        map_.remove(p);
      }
    }
  }
  [[nodiscard]] bool insert(size_t bit) {
    uint32_t word = bit / BitsPerWord;
    WordType mask = bitMask(bit);
    auto p = map_.lookupForAdd(word);
    if (p) {
      p->value() |= mask;
      return true;
    }
    return map_.add(p, word, mask);
  }

  bool empty() const { return map_.empty(); }

  [[nodiscard]] bool insertAll(const SparseBitSet& other) {
    for (Range r(other.map_.all()); !r.empty(); r.popFront()) {
      auto index = r.front().key();
      WordType bits = r.front().value();
      MOZ_ASSERT(bits);
      auto p = map_.lookupForAdd(index);
      if (p) {
        p->value() |= bits;
      } else {
        if (!map_.add(p, index, bits)) {
          return false;
        }
      }
    }
    return true;
  }
};

// Iterates over the set bits in a SparseBitSet. For example:
//
//   using Set = SparseBitSet<AllocPolicy>;
//   Set set;
//   ...
//   for (Set::Iterator iter(set); iter; ++iter) {
//     MOZ_ASSERT(set.contains(*iter));
//   }
template <typename AllocPolicy>
class SparseBitSet<AllocPolicy>::Iterator {
#ifdef DEBUG
  SparseBitSet& bitSet_;
#endif
  SparseBitSet::Range range_;
  WordType currentWord_ = 0;
  // Index of a 1-bit in the SparseBitSet. This is the value returned by
  // |*iter|.
  size_t index_ = 0;

  bool done() const { return range_.empty(); }

  void skipZeroBits() {
    MOZ_ASSERT(!done());
    MOZ_ASSERT(currentWord_ != 0);
    auto numZeroes = mozilla::CountTrailingZeroes32(currentWord_);
    index_ += numZeroes;
    currentWord_ >>= numZeroes;
  }

 public:
  explicit Iterator(SparseBitSet& bitSet)
      :
#ifdef DEBUG
        bitSet_(bitSet),
#endif
        range_(bitSet.map_.all()) {
    if (!range_.empty()) {
      index_ = range_.front().key() * BitsPerWord;
      currentWord_ = range_.front().value();
      skipZeroBits();
    }
  }

  size_t operator*() const {
    MOZ_ASSERT(!done());
    MOZ_ASSERT(bitSet_.contains(index_));
    return index_;
  }

  explicit operator bool() const { return !done(); }

  void operator++() {
    MOZ_ASSERT(!done());
    currentWord_ >>= 1;
    if (currentWord_ == 0) {
      range_.popFront();
      if (range_.empty()) {
        // Done iterating.
        return;
      }
      index_ = range_.front().key() * BitsPerWord;
      currentWord_ = range_.front().value();
    } else {
      index_++;
    }
    skipZeroBits();
  }
};

}  // namespace js::jit

#endif /* jit_SparseBitSet_h */
