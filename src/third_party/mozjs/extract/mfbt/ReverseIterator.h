/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* An iterator that acts like another iterator, but iterating in
 * the negative direction. (Note that not all iterators can iterate
 * in the negative direction.) */

#ifndef mozilla_ReverseIterator_h
#define mozilla_ReverseIterator_h

#include <utility>

#include "mozilla/Attributes.h"

namespace mozilla {

// This should only be used in cases where std::reverse_iterator cannot be used,
// because the underlying iterator is not a proper bidirectional iterator, but
// rather, e.g., a stashing iterator such as IntegerIterator. It is less
// efficient than std::reverse_iterator for proper bidirectional iterators.
template <typename IteratorT>
class ReverseIterator {
 public:
  using value_type = typename IteratorT::value_type;
  using pointer = typename IteratorT::pointer;
  using reference = typename IteratorT::reference;
  using difference_type = typename IteratorT::difference_type;
  using iterator_category = typename IteratorT::iterator_category;

  explicit ReverseIterator(IteratorT aIter) : mCurrent(std::move(aIter)) {}

  // The return type is not reference, but rather the return type of
  // Iterator::operator*(), which might be value_type, to allow this to work
  // with stashing iterators such as IntegerIterator, see also Bug 1175485.
  decltype(*std::declval<IteratorT>()) operator*() const {
    IteratorT tmp = mCurrent;
    return *--tmp;
  }

  /* Difference operator */
  difference_type operator-(const ReverseIterator& aOther) const {
    return aOther.mCurrent - mCurrent;
  }

  /* Increments and decrements operators */

  ReverseIterator& operator++() {
    --mCurrent;
    return *this;
  }
  ReverseIterator& operator--() {
    ++mCurrent;
    return *this;
  }
  ReverseIterator operator++(int) {
    auto ret = *this;
    mCurrent--;
    return ret;
  }
  ReverseIterator operator--(int) {
    auto ret = *this;
    mCurrent++;
    return ret;
  }

  /* Comparison operators */

  template <typename Iterator1, typename Iterator2>
  friend bool operator==(const ReverseIterator<Iterator1>& aIter1,
                         const ReverseIterator<Iterator2>& aIter2);
  template <typename Iterator1, typename Iterator2>
  friend bool operator!=(const ReverseIterator<Iterator1>& aIter1,
                         const ReverseIterator<Iterator2>& aIter2);
  template <typename Iterator1, typename Iterator2>
  friend bool operator<(const ReverseIterator<Iterator1>& aIter1,
                        const ReverseIterator<Iterator2>& aIter2);
  template <typename Iterator1, typename Iterator2>
  friend bool operator<=(const ReverseIterator<Iterator1>& aIter1,
                         const ReverseIterator<Iterator2>& aIter2);
  template <typename Iterator1, typename Iterator2>
  friend bool operator>(const ReverseIterator<Iterator1>& aIter1,
                        const ReverseIterator<Iterator2>& aIter2);
  template <typename Iterator1, typename Iterator2>
  friend bool operator>=(const ReverseIterator<Iterator1>& aIter1,
                         const ReverseIterator<Iterator2>& aIter2);

 private:
  IteratorT mCurrent;
};

template <typename Iterator1, typename Iterator2>
bool operator==(const ReverseIterator<Iterator1>& aIter1,
                const ReverseIterator<Iterator2>& aIter2) {
  return aIter1.mCurrent == aIter2.mCurrent;
}

template <typename Iterator1, typename Iterator2>
bool operator!=(const ReverseIterator<Iterator1>& aIter1,
                const ReverseIterator<Iterator2>& aIter2) {
  return aIter1.mCurrent != aIter2.mCurrent;
}

template <typename Iterator1, typename Iterator2>
bool operator<(const ReverseIterator<Iterator1>& aIter1,
               const ReverseIterator<Iterator2>& aIter2) {
  return aIter1.mCurrent > aIter2.mCurrent;
}

template <typename Iterator1, typename Iterator2>
bool operator<=(const ReverseIterator<Iterator1>& aIter1,
                const ReverseIterator<Iterator2>& aIter2) {
  return aIter1.mCurrent >= aIter2.mCurrent;
}

template <typename Iterator1, typename Iterator2>
bool operator>(const ReverseIterator<Iterator1>& aIter1,
               const ReverseIterator<Iterator2>& aIter2) {
  return aIter1.mCurrent < aIter2.mCurrent;
}

template <typename Iterator1, typename Iterator2>
bool operator>=(const ReverseIterator<Iterator1>& aIter1,
                const ReverseIterator<Iterator2>& aIter2) {
  return aIter1.mCurrent <= aIter2.mCurrent;
}

namespace detail {

template <typename IteratorT,
          typename ReverseIteratorT = ReverseIterator<IteratorT>>
class IteratorRange {
 public:
  typedef IteratorT iterator;
  typedef IteratorT const_iterator;
  typedef ReverseIteratorT reverse_iterator;
  typedef ReverseIteratorT const_reverse_iterator;

  IteratorRange(IteratorT aIterBegin, IteratorT aIterEnd)
      : mIterBegin(std::move(aIterBegin)), mIterEnd(std::move(aIterEnd)) {}

  iterator begin() const { return mIterBegin; }
  const_iterator cbegin() const { return begin(); }
  iterator end() const { return mIterEnd; }
  const_iterator cend() const { return end(); }
  reverse_iterator rbegin() const { return reverse_iterator(mIterEnd); }
  const_reverse_iterator crbegin() const { return rbegin(); }
  reverse_iterator rend() const { return reverse_iterator(mIterBegin); }
  const_reverse_iterator crend() const { return rend(); }

  IteratorT mIterBegin;
  IteratorT mIterEnd;
};

}  // namespace detail

template <typename Range>
detail::IteratorRange<typename Range::reverse_iterator> Reversed(
    Range& aRange) {
  return {aRange.rbegin(), aRange.rend()};
}

template <typename Range>
detail::IteratorRange<typename Range::const_reverse_iterator> Reversed(
    const Range& aRange) {
  return {aRange.rbegin(), aRange.rend()};
}

}  // namespace mozilla

#endif  // mozilla_ReverseIterator_h
