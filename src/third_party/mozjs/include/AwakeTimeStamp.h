/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_AwakeTimeStamp_h
#define mozilla_AwakeTimeStamp_h

#include <stdint.h>
#include <inttypes.h>
#include <mozilla/Types.h>
#include "mozilla/Assertions.h"

namespace mozilla {

class AwakeTimeDuration;

// Conceptually like mozilla::TimeStamp, but only increments when the device is
// awake, on all platforms, and with a restricted API.
//
// It is always valid, there is no way to acquire an AwakeTimeStamp that is not
// valid, unlike TimeStamp that can be null.
//
// Some arithmetic and ordering operations are supported, when they make sense.
//
// This timestamp shouldn't be considered to be high-resolution, and is suitable
// to measure time from a hundred of milliseconds (because of Windows
// limitations).
class AwakeTimeStamp {
 public:
  MFBT_API static AwakeTimeStamp NowLoRes();
  MFBT_API void operator+=(const AwakeTimeDuration& aOther);
  MFBT_API void operator-=(const AwakeTimeDuration& aOther);
  MFBT_API bool operator<(const AwakeTimeStamp& aOther) const {
    return mValueUs < aOther.mValueUs;
  }
  MFBT_API bool operator<=(const AwakeTimeStamp& aOther) const {
    return mValueUs <= aOther.mValueUs;
  }
  MFBT_API bool operator>=(const AwakeTimeStamp& aOther) const {
    return mValueUs >= aOther.mValueUs;
  }
  MFBT_API bool operator>(const AwakeTimeStamp& aOther) const {
    return mValueUs > aOther.mValueUs;
  }
  MFBT_API bool operator==(const AwakeTimeStamp& aOther) const {
    return mValueUs == aOther.mValueUs;
  }
  MFBT_API bool operator!=(const AwakeTimeStamp& aOther) const {
    return !(*this == aOther);
  }
  MFBT_API AwakeTimeDuration operator-(AwakeTimeStamp const& aOther) const;
  MFBT_API AwakeTimeStamp operator+(const AwakeTimeDuration& aDuration) const;

 private:
  explicit AwakeTimeStamp(uint64_t aValueUs) : mValueUs(aValueUs) {}

  uint64_t mValueUs;
};

// A duration, only counting the time the computer was awake.
//
// Can be obtained via subtracting two AwakeTimeStamp, or default-contructed to
// mean a empty duration.
//
// Arithmetic and ordering operations are defined when they make sense.
class AwakeTimeDuration {
 public:
  MFBT_API AwakeTimeDuration() : mValueUs(0) {}

  MFBT_API double ToSeconds() const;
  MFBT_API double ToMilliseconds() const;
  MFBT_API double ToMicroseconds() const;
  MFBT_API void operator+=(const AwakeTimeDuration& aDuration) {
    mValueUs += aDuration.mValueUs;
  }
  MFBT_API AwakeTimeDuration operator+(const AwakeTimeDuration& aOther) const {
    return AwakeTimeDuration(mValueUs + aOther.mValueUs);
  }
  MFBT_API AwakeTimeDuration operator-(const AwakeTimeDuration& aOther) const {
    MOZ_ASSERT(mValueUs >= aOther.mValueUs);
    return AwakeTimeDuration(mValueUs - aOther.mValueUs);
  }
  MFBT_API void operator-=(const AwakeTimeDuration& aOther) {
    MOZ_ASSERT(mValueUs >= aOther.mValueUs);
    mValueUs -= aOther.mValueUs;
  }
  MFBT_API bool operator<(const AwakeTimeDuration& aOther) const {
    return mValueUs < aOther.mValueUs;
  }
  MFBT_API bool operator<=(const AwakeTimeDuration& aOther) const {
    return mValueUs <= aOther.mValueUs;
  }
  MFBT_API bool operator>=(const AwakeTimeDuration& aOther) const {
    return mValueUs >= aOther.mValueUs;
  }
  MFBT_API bool operator>(const AwakeTimeDuration& aOther) const {
    return mValueUs > aOther.mValueUs;
  }
  MFBT_API bool operator==(const AwakeTimeDuration& aOther) const {
    return mValueUs == aOther.mValueUs;
  }
  MFBT_API bool operator!=(const AwakeTimeDuration& aOther) const {
    return !(*this == aOther);
  }

 private:
  friend AwakeTimeStamp;
  // Not using a default value because we want this private, but allow creating
  // duration that are empty.
  explicit AwakeTimeDuration(uint64_t aValueUs) : mValueUs(aValueUs) {}

  uint64_t mValueUs;
};

};  // namespace mozilla

#endif  // mozilla_AwakeTimeStamp_h
