/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Calculate the rolling mean of a series of values. */

#ifndef mozilla_RollingMean_h_
#define mozilla_RollingMean_h_

#include "mozilla/Assertions.h"
#include "mozilla/Vector.h"

#include <stddef.h>
#include <type_traits>

namespace mozilla {

/**
 * RollingMean<T> calculates a rolling mean of the values it is given. It
 * accumulates the total as values are added and removed. The second type
 * argument S specifies the type of the total. This may need to be a bigger
 * type in order to maintain that the sum of all values in the average doesn't
 * exceed the maximum input value.
 *
 * WARNING: Float types are not supported due to rounding errors.
 */
template <typename T, typename S>
class RollingMean {
 private:
  size_t mInsertIndex;
  size_t mMaxValues;
  Vector<T> mValues;
  S mTotal;

 public:
  static_assert(!std::is_floating_point_v<T>,
                "floating-point types are unsupported due to rounding "
                "errors");

  explicit RollingMean(size_t aMaxValues)
      : mInsertIndex(0), mMaxValues(aMaxValues), mTotal(0) {
    MOZ_ASSERT(aMaxValues > 0);
  }

  RollingMean& operator=(RollingMean&& aOther) = default;

  /**
   * Insert a value into the rolling mean.
   */
  bool insert(T aValue) {
    MOZ_ASSERT(mValues.length() <= mMaxValues);

    if (mValues.length() == mMaxValues) {
      mTotal = mTotal - mValues[mInsertIndex] + aValue;
      mValues[mInsertIndex] = aValue;
    } else {
      if (!mValues.append(aValue)) {
        return false;
      }
      mTotal = mTotal + aValue;
    }

    mInsertIndex = (mInsertIndex + 1) % mMaxValues;
    return true;
  }

  /**
   * Calculate the rolling mean.
   */
  T mean() const {
    MOZ_ASSERT(!empty());
    return T(mTotal / int64_t(mValues.length()));
  }

  bool empty() const { return mValues.empty(); }

  /**
   * Remove all values from the rolling mean.
   */
  void clear() {
    mValues.clear();
    mInsertIndex = 0;
    mTotal = T(0);
  }

  size_t maxValues() const { return mMaxValues; }
};

}  // namespace mozilla

#endif  // mozilla_RollingMean_h_
