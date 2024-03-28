// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_INTERNAL_RANGE_TRACKER_H_
#define TCMALLOC_INTERNAL_RANGE_TRACKER_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <algorithm>
#include <limits>
#include <type_traits>

#include "absl/numeric/bits.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Keeps a bitmap of some fixed size (N bits).
template <size_t N>
class Bitmap {
 public:
  constexpr Bitmap() : bits_{} {}

  size_t size() const { return N; }
  bool GetBit(size_t i) const;

  void SetBit(size_t i);
  void ClearBit(size_t i);

  // Returns the number of set bits [index, ..., index + n - 1].
  size_t CountBits(size_t index, size_t n) const;

  // Returns whether the bitmap is entirely zero or not.
  bool IsZero() const;

  // Equivalent to SetBit on bits [index, index + 1, ... index + n - 1].
  void SetRange(size_t index, size_t n);
  void ClearRange(size_t index, size_t n);

  // Clears the lowest set bit. Special case is faster than more flexible code.
  void ClearLowestBit();

  // If there is at least one free range at or after <start>,
  // put it in *index, *length and return true; else return false.
  bool NextFreeRange(size_t start, size_t* index, size_t* length) const;

  // Returns index of the first {true, false} bit >= index, or N if none.
  size_t FindSet(size_t index) const;
  size_t FindClear(size_t index) const;

  // Returns index of the first {set, clear} bit in [index, 0] or -1 if none.
  ssize_t FindSetBackwards(size_t index) const;
  ssize_t FindClearBackwards(size_t index) const;

  void Clear();

 private:
  static constexpr size_t kWordSize = sizeof(size_t) * 8;
  static constexpr size_t kWords = (N + kWordSize - 1) / kWordSize;
  static constexpr size_t kDeadBits = kWordSize * kWords - N;

  size_t bits_[kWords];

  size_t CountWordBits(size_t i, size_t from, size_t to) const;

  template <bool Value>
  void SetWordBits(size_t i, size_t from, size_t to);
  template <bool Value>
  void SetRangeValue(size_t index, size_t n);

  template <bool Goal>
  size_t FindValue(size_t index) const;
  template <bool Goal>
  ssize_t FindValueBackwards(size_t index) const;
};

// Tracks allocations in a range of items of fixed size.  Supports
// finding an unset range of a given length, while keeping track of
// the largest remaining unmarked length.
template <size_t N>
class RangeTracker {
 public:
  constexpr RangeTracker()
      : bits_{}, longest_free_(N), nused_(0), nallocs_(0) {}

  size_t size() const;
  // Number of bits marked
  size_t used() const;
  // Number of bits clear
  size_t total_free() const;
  // Longest contiguous range of clear bits.
  size_t longest_free() const;
  // Count of live allocations.
  size_t allocs() const;

  // REQUIRES: there is a free range of at least n bits
  // (i.e. n <= longest_free()).
  //
  // Finds and marks n free bits, returning index of the first bit.  Chooses by
  // best fit.
  size_t FindAndMark(size_t n);

  // REQUIRES: the range [index, index + n) is fully marked, and
  // was the returned value from a call to FindAndMark.
  // Unmarks it.
  void Unmark(size_t index, size_t n);
  // If there is at least one free range at or after <start>,
  // put it in *index, *length and return true; else return false.
  bool NextFreeRange(size_t start, size_t* index, size_t* length) const;

  void Clear();

 private:
  Bitmap<N> bits_;

  // Computes the smallest unsigned type that can hold the constant N.
  class UnsignedTypeFittingSize {
   private:
    static_assert(N <= std::numeric_limits<uint64_t>::max(),
                  "size_t more than 64 bits??");
    template <typename T>
    static constexpr bool Fit() {
      return N <= std::numeric_limits<T>::max();
    }
    struct U32 {
      using type =
          typename std::conditional<Fit<uint32_t>(), uint32_t, uint64_t>::type;
    };

    struct U16 {
      using type = typename std::conditional<Fit<uint16_t>(), uint16_t,
                                             typename U32::type>::type;
    };

    struct U8 {
      using type = typename std::conditional<Fit<uint8_t>(), uint8_t,
                                             typename U16::type>::type;
    };

   public:
    using type = typename U8::type;
  };

  // we keep various stats in the range [0, N]; make them as small as possible.
  using Count = typename UnsignedTypeFittingSize::type;

  Count longest_free_;
  Count nused_;
  Count nallocs_;
};

template <size_t N>
inline size_t RangeTracker<N>::size() const {
  return bits_.size();
}

template <size_t N>
inline size_t RangeTracker<N>::used() const {
  return nused_;
}

template <size_t N>
inline size_t RangeTracker<N>::total_free() const {
  return N - used();
}

template <size_t N>
inline size_t RangeTracker<N>::longest_free() const {
  return longest_free_;
}

template <size_t N>
inline size_t RangeTracker<N>::allocs() const {
  return nallocs_;
}

template <size_t N>
inline size_t RangeTracker<N>::FindAndMark(size_t n) {
  TC_ASSERT_GT(n, 0);

  // We keep the two longest ranges in the bitmap since we might allocate
  // from one.
  size_t longest_len = 0;
  size_t second_len = 0;

  // the best (shortest) range we could use
  // TODO(b/134691947): shortest? lowest-addressed?
  size_t best_index = N;
  size_t best_len = 2 * N;
  // Iterate over free ranges:
  size_t index = 0, len;

  while (bits_.NextFreeRange(index, &index, &len)) {
    if (len > longest_len) {
      second_len = longest_len;
      longest_len = len;
    } else if (len > second_len) {
      second_len = len;
    }

    if (len >= n && len < best_len) {
      best_index = index;
      best_len = len;
    }

    index += len;
  }

  TC_CHECK_LT(best_index, N);
  bits_.SetRange(best_index, n);

  if (best_len == longest_len) {
    longest_len -= n;
    if (longest_len < second_len) longest_len = second_len;
  }

  longest_free_ = longest_len;
  nused_ += n;
  nallocs_++;
  return best_index;
}

// REQUIRES: the range [index, index + n) is fully marked.
// Unmarks it.
template <size_t N>
inline void RangeTracker<N>::Unmark(size_t index, size_t n) {
  TC_ASSERT(bits_.FindClear(index) >= index + n);
  bits_.ClearRange(index, n);
  nused_ -= n;
  nallocs_--;

  // We just opened up a new free range--it might be the longest.
  size_t lim = bits_.FindSet(index + n - 1);
  index = bits_.FindSetBackwards(index) + 1;
  n = lim - index;
  if (n > longest_free()) {
    longest_free_ = n;
  }
}

// If there is at least one free range at or after <start>,
// put it in *index, *length and return true; else return false.
template <size_t N>
inline bool RangeTracker<N>::NextFreeRange(size_t start, size_t* index,
                                           size_t* length) const {
  return bits_.NextFreeRange(start, index, length);
}

template <size_t N>
inline void RangeTracker<N>::Clear() {
  bits_.Clear();
  nallocs_ = 0;
  nused_ = 0;
  longest_free_ = N;
}

// Count the set bits [from, to) in the i-th word to Value.
template <size_t N>
inline size_t Bitmap<N>::CountWordBits(size_t i, size_t from, size_t to) const {
  TC_ASSERT_LT(from, kWordSize);
  TC_ASSERT_LE(to, kWordSize);
  const size_t all_ones = ~static_cast<size_t>(0);
  // how many bits are we setting?
  const size_t n = to - from;
  TC_ASSERT(0 < n && n <= kWordSize);
  const size_t mask = (all_ones >> (kWordSize - n)) << from;

  ASSUME(i < kWords);
  return absl::popcount(bits_[i] & mask);
}

// Set the bits [from, to) in the i-th word to Value.
template <size_t N>
template <bool Value>
inline void Bitmap<N>::SetWordBits(size_t i, size_t from, size_t to) {
  TC_ASSERT_LT(from, kWordSize);
  TC_ASSERT_LE(to, kWordSize);
  const size_t all_ones = ~static_cast<size_t>(0);
  // how many bits are we setting?
  const size_t n = to - from;
  TC_ASSERT(n > 0 && n <= kWordSize);
  const size_t mask = (all_ones >> (kWordSize - n)) << from;
  ASSUME(i < kWords);
  if (Value) {
    bits_[i] |= mask;
  } else {
    bits_[i] &= ~mask;
  }
}

template <size_t N>
inline bool Bitmap<N>::GetBit(size_t i) const {
  TC_ASSERT_LT(i, N);
  size_t word = i / kWordSize;
  size_t offset = i % kWordSize;
  ASSUME(word < kWords);
  return bits_[word] & (size_t{1} << offset);
}

template <size_t N>
inline void Bitmap<N>::SetBit(size_t i) {
  TC_ASSERT_LT(i, N);
  size_t word = i / kWordSize;
  size_t offset = i % kWordSize;
  ASSUME(word < kWords);
  bits_[word] |= (size_t{1} << offset);
}

template <size_t N>
inline void Bitmap<N>::ClearBit(size_t i) {
  TC_ASSERT_LT(i, N);
  size_t word = i / kWordSize;
  size_t offset = i % kWordSize;
  ASSUME(word < kWords);
  bits_[word] &= ~(size_t{1} << offset);
}

template <size_t N>
inline size_t Bitmap<N>::CountBits(size_t index, size_t n) const {
  ASSUME(index + n <= N);
  size_t count = 0;
  if (n == 0) {
    return count;
  }

  size_t word = index / kWordSize;
  size_t offset = index % kWordSize;
  size_t k = std::min(offset + n, kWordSize);
  count += CountWordBits(word, offset, k);
  n -= k - offset;
  while (n > 0) {
    word++;
    k = std::min(n, kWordSize);
    count += CountWordBits(word, 0, k);
    n -= k;
  }

  return count;
}

template <size_t N>
inline bool Bitmap<N>::IsZero() const {
  for (int i = 0; i < kWords; ++i) {
    if (bits_[i] != 0) {
      return false;
    }
  }
  return true;
}

template <size_t N>
inline void Bitmap<N>::SetRange(size_t index, size_t n) {
  SetRangeValue<true>(index, n);
}

template <size_t N>
inline void Bitmap<N>::ClearRange(size_t index, size_t n) {
  SetRangeValue<false>(index, n);
}

template <size_t N>
inline void Bitmap<N>::ClearLowestBit() {
  for (int i = 0; i < kWords; ++i) {
    if (bits_[i] != 0) {
      bits_[i] &= bits_[i] - 1;
      break;
    }
  }
}

template <size_t N>
template <bool Value>
inline void Bitmap<N>::SetRangeValue(size_t index, size_t n) {
  TC_ASSERT_LE(index + n, N);
  size_t word = index / kWordSize;
  size_t offset = index % kWordSize;
  size_t k = offset + n;
  if (k > kWordSize) k = kWordSize;
  SetWordBits<Value>(word, offset, k);
  n -= k - offset;
  while (n > 0) {
    word++;
    k = n;
    if (k > kWordSize) k = kWordSize;
    SetWordBits<Value>(word, 0, k);
    n -= k;
  }
}

template <size_t N>
inline bool Bitmap<N>::NextFreeRange(size_t start, size_t* index,
                                     size_t* length) const {
  if (start >= N) return false;
  size_t i = FindClear(start);
  if (i == N) return false;
  size_t j = FindSet(i);
  *index = i;
  *length = j - i;
  return true;
}

template <size_t N>
inline size_t Bitmap<N>::FindSet(size_t index) const {
  return FindValue<true>(index);
}

template <size_t N>
inline size_t Bitmap<N>::FindClear(size_t index) const {
  return FindValue<false>(index);
}

template <size_t N>
inline ssize_t Bitmap<N>::FindSetBackwards(size_t index) const {
  return FindValueBackwards<true>(index);
}

template <size_t N>
inline ssize_t Bitmap<N>::FindClearBackwards(size_t index) const {
  return FindValueBackwards<false>(index);
}

template <size_t N>
inline void Bitmap<N>::Clear() {
  for (int i = 0; i < kWords; ++i) {
    bits_[i] = 0;
  }
}

template <size_t N>
template <bool Goal>
inline size_t Bitmap<N>::FindValue(size_t index) const {
  TC_ASSERT_LT(index, N);
  size_t offset = index % kWordSize;
  size_t word = index / kWordSize;
  ASSUME(word < kWords);
  size_t here = bits_[word];
  if (!Goal) here = ~here;
  size_t mask = ~static_cast<size_t>(0) << offset;
  here &= mask;
  while (here == 0) {
    ++word;
    if (word >= kWords) {
      return N;
    }
    here = bits_[word];
    if (!Goal) here = ~here;
  }

  word *= kWordSize;
  ASSUME(here != 0);
  size_t ret = absl::countr_zero(here) + word;
  if (kDeadBits > 0) {
    if (ret > N) ret = N;
  }
  return ret;
}

template <size_t N>
template <bool Goal>
inline ssize_t Bitmap<N>::FindValueBackwards(size_t index) const {
  TC_ASSERT_LT(index, N);
  size_t offset = index % kWordSize;
  ssize_t word = index / kWordSize;
  ASSUME(word < kWords);
  size_t here = bits_[word];
  if (!Goal) here = ~here;
  size_t mask = (static_cast<size_t>(2) << offset) - 1;
  here &= mask;
  while (here == 0) {
    --word;
    if (word < 0) {
      return -1;
    }
    here = bits_[word];
    if (!Goal) here = ~here;
  }

  word *= kWordSize;
  ASSUME(here != 0);
  size_t ret = absl::bit_width(here) - 1 + word;
  return ret;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_RANGE_TRACKER_H_
