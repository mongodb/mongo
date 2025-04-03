/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_BinarySearch_h
#define mozilla_BinarySearch_h

#include "mozilla/Assertions.h"
#include "mozilla/CompactPair.h"

#include <stddef.h>

namespace mozilla {

/*
 * The BinarySearch() algorithm searches the given container |aContainer| over
 * the sorted index range [aBegin, aEnd) for an index |i| where
 * |aContainer[i] == aTarget|.
 * If such an index |i| is found, BinarySearch returns |true| and the index is
 * returned via the outparam |aMatchOrInsertionPoint|. If no index is found,
 * BinarySearch returns |false| and the outparam returns the first index in
 * [aBegin, aEnd] where |aTarget| can be inserted to maintain sorted order.
 *
 * Example:
 *
 *   Vector<int> sortedInts = ...
 *
 *   size_t match;
 *   if (BinarySearch(sortedInts, 0, sortedInts.length(), 13, &match)) {
 *     printf("found 13 at %lu\n", match);
 *   }
 *
 * The BinarySearchIf() version behaves similarly, but takes |aComparator|, a
 * functor to compare the values with, instead of a value to find.
 * That functor should take one argument - the value to compare - and return an
 * |int| with the comparison result:
 *
 *   * 0, if the argument is equal to,
 *   * less than 0, if the argument is greater than,
 *   * greater than 0, if the argument is less than
 *
 * the value.
 *
 * Example:
 *
 *   struct Comparator {
 *     int operator()(int aVal) const {
 *       if (mTarget < aVal) { return -1; }
 *       if (mTarget > aVal) { return 1; }
 *       return 0;
 *     }
 *     explicit Comparator(int aTarget) : mTarget(aTarget) {}
 *     const int mTarget;
 *   };
 *
 *   Vector<int> sortedInts = ...
 *
 *   size_t match;
 *   if (BinarySearchIf(sortedInts, 0, sortedInts.length(), Comparator(13),
 * &match)) { printf("found 13 at %lu\n", match);
 *   }
 *
 */

template <typename Container, typename Comparator>
bool BinarySearchIf(const Container& aContainer, size_t aBegin, size_t aEnd,
                    const Comparator& aCompare,
                    size_t* aMatchOrInsertionPoint) {
  MOZ_ASSERT(aBegin <= aEnd);

  size_t low = aBegin;
  size_t high = aEnd;
  while (high != low) {
    size_t middle = low + (high - low) / 2;

    // Allow any intermediate type so long as it provides a suitable ordering
    // relation.
    const int result = aCompare(aContainer[middle]);

    if (result == 0) {
      *aMatchOrInsertionPoint = middle;
      return true;
    }

    if (result < 0) {
      high = middle;
    } else {
      low = middle + 1;
    }
  }

  *aMatchOrInsertionPoint = low;
  return false;
}

namespace detail {

template <class T>
class BinarySearchDefaultComparator {
 public:
  explicit BinarySearchDefaultComparator(const T& aTarget) : mTarget(aTarget) {}

  template <class U>
  int operator()(const U& aVal) const {
    if (mTarget == aVal) {
      return 0;
    }

    if (mTarget < aVal) {
      return -1;
    }

    return 1;
  }

 private:
  const T& mTarget;
};

}  // namespace detail

template <typename Container, typename T>
bool BinarySearch(const Container& aContainer, size_t aBegin, size_t aEnd,
                  T aTarget, size_t* aMatchOrInsertionPoint) {
  return BinarySearchIf(aContainer, aBegin, aEnd,
                        detail::BinarySearchDefaultComparator<T>(aTarget),
                        aMatchOrInsertionPoint);
}

/*
 * LowerBound(), UpperBound(), and EqualRange() are equivalent to
 * std::lower_bound(), std::upper_bound(), and std::equal_range() respectively.
 *
 * LowerBound() returns an index pointing to the first element in the range
 * in which each element is considered *not less than* the given value passed
 * via |aCompare|, or the length of |aContainer| if no such element is found.
 *
 * UpperBound() returns an index pointing to the first element in the range
 * in which each element is considered *greater than* the given value passed
 * via |aCompare|, or the length of |aContainer| if no such element is found.
 *
 * EqualRange() returns a range [first, second) containing all elements are
 * considered equivalent to the given value via |aCompare|.  If you need
 * either the first or last index of the range, LowerBound() or UpperBound(),
 * which is slightly faster than EqualRange(), should suffice.
 *
 * Example (another example is given in TestBinarySearch.cpp):
 *
 *   Vector<const char*> sortedStrings = ...
 *
 *   struct Comparator {
 *     const nsACString& mStr;
 *     explicit Comparator(const nsACString& aStr) : mStr(aStr) {}
 *     int32_t operator()(const char* aVal) const {
 *       return Compare(mStr, nsDependentCString(aVal));
 *     }
 *   };
 *
 *   auto bounds = EqualRange(sortedStrings, 0, sortedStrings.length(),
 *                            Comparator("needle I'm looking for"_ns));
 *   printf("Found the range [%zd %zd)\n", bounds.first(), bounds.second());
 *
 */
template <typename Container, typename Comparator>
size_t LowerBound(const Container& aContainer, size_t aBegin, size_t aEnd,
                  const Comparator& aCompare) {
  MOZ_ASSERT(aBegin <= aEnd);

  size_t low = aBegin;
  size_t high = aEnd;
  while (high != low) {
    size_t middle = low + (high - low) / 2;

    // Allow any intermediate type so long as it provides a suitable ordering
    // relation.
    const int result = aCompare(aContainer[middle]);

    // The range returning from LowerBound does include elements
    // equivalent to the given value i.e. aCompare(element) == 0
    if (result <= 0) {
      high = middle;
    } else {
      low = middle + 1;
    }
  }

  return low;
}

template <typename Container, typename Comparator>
size_t UpperBound(const Container& aContainer, size_t aBegin, size_t aEnd,
                  const Comparator& aCompare) {
  MOZ_ASSERT(aBegin <= aEnd);

  size_t low = aBegin;
  size_t high = aEnd;
  while (high != low) {
    size_t middle = low + (high - low) / 2;

    // Allow any intermediate type so long as it provides a suitable ordering
    // relation.
    const int result = aCompare(aContainer[middle]);

    // The range returning from UpperBound does NOT include elements
    // equivalent to the given value i.e. aCompare(element) == 0
    if (result < 0) {
      high = middle;
    } else {
      low = middle + 1;
    }
  }

  return high;
}

template <typename Container, typename Comparator>
CompactPair<size_t, size_t> EqualRange(const Container& aContainer,
                                       size_t aBegin, size_t aEnd,
                                       const Comparator& aCompare) {
  MOZ_ASSERT(aBegin <= aEnd);

  size_t low = aBegin;
  size_t high = aEnd;
  while (high != low) {
    size_t middle = low + (high - low) / 2;

    // Allow any intermediate type so long as it provides a suitable ordering
    // relation.
    const int result = aCompare(aContainer[middle]);

    if (result < 0) {
      high = middle;
    } else if (result > 0) {
      low = middle + 1;
    } else {
      return MakeCompactPair(
          LowerBound(aContainer, low, middle, aCompare),
          UpperBound(aContainer, middle + 1, high, aCompare));
    }
  }

  return MakeCompactPair(low, high);
}

}  // namespace mozilla

#endif  // mozilla_BinarySearch_h
