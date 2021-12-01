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

#include "mozilla/Attributes.h"
#include "mozilla/TypeTraits.h"

namespace mozilla {

template<typename IteratorT>
class ReverseIterator
{
public:
  template<typename Iterator>
  explicit ReverseIterator(Iterator aIter)
    : mCurrent(aIter) { }

  template<typename Iterator>
  MOZ_IMPLICIT ReverseIterator(const ReverseIterator<Iterator>& aOther)
    : mCurrent(aOther.mCurrent) { }

  decltype(*DeclVal<IteratorT>()) operator*() const
  {
    IteratorT tmp = mCurrent;
    return *--tmp;
  }

  /* Increments and decrements operators */

  ReverseIterator& operator++() { --mCurrent; return *this; }
  ReverseIterator& operator--() { ++mCurrent; return *this; }
  ReverseIterator operator++(int) { auto ret = *this; mCurrent--; return ret; }
  ReverseIterator operator--(int) { auto ret = *this; mCurrent++; return ret; }

  /* Comparison operators */

  template<typename Iterator1, typename Iterator2>
  friend bool operator==(const ReverseIterator<Iterator1>& aIter1,
                         const ReverseIterator<Iterator2>& aIter2);
  template<typename Iterator1, typename Iterator2>
  friend bool operator!=(const ReverseIterator<Iterator1>& aIter1,
                         const ReverseIterator<Iterator2>& aIter2);
  template<typename Iterator1, typename Iterator2>
  friend bool operator<(const ReverseIterator<Iterator1>& aIter1,
                        const ReverseIterator<Iterator2>& aIter2);
  template<typename Iterator1, typename Iterator2>
  friend bool operator<=(const ReverseIterator<Iterator1>& aIter1,
                         const ReverseIterator<Iterator2>& aIter2);
  template<typename Iterator1, typename Iterator2>
  friend bool operator>(const ReverseIterator<Iterator1>& aIter1,
                        const ReverseIterator<Iterator2>& aIter2);
  template<typename Iterator1, typename Iterator2>
  friend bool operator>=(const ReverseIterator<Iterator1>& aIter1,
                         const ReverseIterator<Iterator2>& aIter2);

private:
  IteratorT mCurrent;
};

template<typename Iterator1, typename Iterator2>
bool
operator==(const ReverseIterator<Iterator1>& aIter1,
           const ReverseIterator<Iterator2>& aIter2)
{
  return aIter1.mCurrent == aIter2.mCurrent;
}

template<typename Iterator1, typename Iterator2>
bool
operator!=(const ReverseIterator<Iterator1>& aIter1,
           const ReverseIterator<Iterator2>& aIter2)
{
  return aIter1.mCurrent != aIter2.mCurrent;
}

template<typename Iterator1, typename Iterator2>
bool
operator<(const ReverseIterator<Iterator1>& aIter1,
          const ReverseIterator<Iterator2>& aIter2)
{
  return aIter1.mCurrent > aIter2.mCurrent;
}

template<typename Iterator1, typename Iterator2>
bool
operator<=(const ReverseIterator<Iterator1>& aIter1,
           const ReverseIterator<Iterator2>& aIter2)
{
  return aIter1.mCurrent >= aIter2.mCurrent;
}

template<typename Iterator1, typename Iterator2>
bool
operator>(const ReverseIterator<Iterator1>& aIter1,
          const ReverseIterator<Iterator2>& aIter2)
{
  return aIter1.mCurrent < aIter2.mCurrent;
}

template<typename Iterator1, typename Iterator2>
bool
operator>=(const ReverseIterator<Iterator1>& aIter1,
           const ReverseIterator<Iterator2>& aIter2)
{
  return aIter1.mCurrent <= aIter2.mCurrent;
}

namespace detail {

template<typename IteratorT>
class IteratorRange
{
public:
  typedef IteratorT iterator;
  typedef IteratorT const_iterator;
  typedef ReverseIterator<IteratorT> reverse_iterator;
  typedef ReverseIterator<IteratorT> const_reverse_iterator;

  template<typename Iterator1, typename Iterator2>
  MOZ_IMPLICIT IteratorRange(Iterator1 aIterBegin, Iterator2 aIterEnd)
    : mIterBegin(aIterBegin), mIterEnd(aIterEnd) { }

  template<typename Iterator>
  MOZ_IMPLICIT IteratorRange(const IteratorRange<Iterator>& aOther)
    : mIterBegin(aOther.mIterBegin), mIterEnd(aOther.mIterEnd) { }

  iterator begin() const { return mIterBegin; }
  const_iterator cbegin() const { return begin(); }
  iterator end() const { return mIterEnd; }
  const_iterator cend() const { return end(); }
  reverse_iterator rbegin() const { return reverse_iterator(mIterEnd); }
  const_reverse_iterator crbegin() const { return rbegin(); }
  reverse_iterator rend() const { return reverse_iterator(mIterBegin); }
  const_reverse_iterator crend() const { return rend(); }

private:
  IteratorT mIterBegin;
  IteratorT mIterEnd;
};

} // namespace detail

template<typename Range>
detail::IteratorRange<typename Range::reverse_iterator>
Reversed(Range& aRange)
{
  return {aRange.rbegin(), aRange.rend()};
}

template<typename Range>
detail::IteratorRange<typename Range::const_reverse_iterator>
Reversed(const Range& aRange)
{
  return {aRange.rbegin(), aRange.rend()};
}

} // namespace mozilla

#endif // mozilla_ReverseIterator_h
