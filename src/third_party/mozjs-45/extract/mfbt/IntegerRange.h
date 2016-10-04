/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Iterator over ranges of integers */

#ifndef mozilla_IntegerRange_h
#define mozilla_IntegerRange_h

#include "mozilla/Assertions.h"
#include "mozilla/ReverseIterator.h"
#include "mozilla/TypeTraits.h"

namespace mozilla {

namespace detail {

template<typename IntTypeT>
class IntegerIterator
{
public:
  template<typename IntType>
  explicit IntegerIterator(IntType aCurrent)
    : mCurrent(aCurrent) { }

  template<typename IntType>
  explicit IntegerIterator(const IntegerIterator<IntType>& aOther)
    : mCurrent(aOther.mCurrent) { }

  IntTypeT operator*() const { return mCurrent; }

  /* Increment and decrement operators */

  IntegerIterator& operator++() { ++mCurrent; return *this; }
  IntegerIterator& operator--() { --mCurrent; return *this; }
  IntegerIterator operator++(int) { auto ret = *this; ++mCurrent; return ret; }
  IntegerIterator operator--(int) { auto ret = *this; --mCurrent; return ret; }

  /* Comparison operators */

  template<typename IntType1, typename IntType2>
  friend bool operator==(const IntegerIterator<IntType1>& aIter1,
                         const IntegerIterator<IntType2>& aIter2);
  template<typename IntType1, typename IntType2>
  friend bool operator!=(const IntegerIterator<IntType1>& aIter1,
                         const IntegerIterator<IntType2>& aIter2);
  template<typename IntType1, typename IntType2>
  friend bool operator<(const IntegerIterator<IntType1>& aIter1,
                        const IntegerIterator<IntType2>& aIter2);
  template<typename IntType1, typename IntType2>
  friend bool operator<=(const IntegerIterator<IntType1>& aIter1,
                         const IntegerIterator<IntType2>& aIter2);
  template<typename IntType1, typename IntType2>
  friend bool operator>(const IntegerIterator<IntType1>& aIter1,
                        const IntegerIterator<IntType2>& aIter2);
  template<typename IntType1, typename IntType2>
  friend bool operator>=(const IntegerIterator<IntType1>& aIter1,
                         const IntegerIterator<IntType2>& aIter2);

private:
  IntTypeT mCurrent;
};

template<typename IntType1, typename IntType2>
bool operator==(const IntegerIterator<IntType1>& aIter1,
                const IntegerIterator<IntType2>& aIter2)
{
  return aIter1.mCurrent == aIter2.mCurrent;
}

template<typename IntType1, typename IntType2>
bool operator!=(const IntegerIterator<IntType1>& aIter1,
                const IntegerIterator<IntType2>& aIter2)
{
  return aIter1.mCurrent != aIter2.mCurrent;
}

template<typename IntType1, typename IntType2>
bool operator<(const IntegerIterator<IntType1>& aIter1,
               const IntegerIterator<IntType2>& aIter2)
{
  return aIter1.mCurrent < aIter2.mCurrent;
}

template<typename IntType1, typename IntType2>
bool operator<=(const IntegerIterator<IntType1>& aIter1,
                const IntegerIterator<IntType2>& aIter2)
{
  return aIter1.mCurrent <= aIter2.mCurrent;
}

template<typename IntType1, typename IntType2>
bool operator>(const IntegerIterator<IntType1>& aIter1,
               const IntegerIterator<IntType2>& aIter2)
{
  return aIter1.mCurrent > aIter2.mCurrent;
}

template<typename IntType1, typename IntType2>
bool operator>=(const IntegerIterator<IntType1>& aIter1,
                const IntegerIterator<IntType2>& aIter2)
{
  return aIter1.mCurrent >= aIter2.mCurrent;
}

template<typename IntTypeT>
class IntegerRange
{
public:
  typedef IntegerIterator<IntTypeT> iterator;
  typedef IntegerIterator<IntTypeT> const_iterator;
  typedef ReverseIterator<IntegerIterator<IntTypeT>> reverse_iterator;
  typedef ReverseIterator<IntegerIterator<IntTypeT>> const_reverse_iterator;

  template<typename IntType>
  explicit IntegerRange(IntType aEnd)
    : mBegin(0), mEnd(aEnd) { }

  template<typename IntType1, typename IntType2>
  IntegerRange(IntType1 aBegin, IntType2 aEnd)
    : mBegin(aBegin), mEnd(aEnd) { }

  iterator begin() const { return iterator(mBegin); }
  const_iterator cbegin() const { return begin(); }
  iterator end() const { return iterator(mEnd); }
  const_iterator cend() const { return end(); }
  reverse_iterator rbegin() const { return reverse_iterator(mEnd); }
  const_reverse_iterator crbegin() const { return rbegin(); }
  reverse_iterator rend() const { return reverse_iterator(mBegin); }
  const_reverse_iterator crend() const { return rend(); }

private:
  IntTypeT mBegin;
  IntTypeT mEnd;
};

template<typename T, bool = IsUnsigned<T>::value>
struct GeqZero
{
  static bool check(T t) {
    return t >= 0;
  }
};

template<typename T>
struct GeqZero<T, true>
{
  static bool check(T t) {
    return true;
  }
};

} // namespace detail

template<typename IntType>
detail::IntegerRange<IntType>
MakeRange(IntType aEnd)
{
  static_assert(IsIntegral<IntType>::value, "value must be integral");
  MOZ_ASSERT(detail::GeqZero<IntType>::check(aEnd),
             "Should never have negative value here");
  return detail::IntegerRange<IntType>(aEnd);
}

template<typename IntType1, typename IntType2>
detail::IntegerRange<IntType2>
MakeRange(IntType1 aBegin, IntType2 aEnd)
{
  static_assert(IsIntegral<IntType1>::value && IsIntegral<IntType2>::value,
                "values must both be integral");
  static_assert(IsSigned<IntType1>::value == IsSigned<IntType2>::value,
                "signed/unsigned mismatch");
  MOZ_ASSERT(aEnd >= aBegin, "End value should be larger than begin value");
  return detail::IntegerRange<IntType2>(aBegin, aEnd);
}

} // namespace mozilla

#endif // mozilla_IntegerRange_h
