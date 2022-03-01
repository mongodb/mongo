/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_TimeStamp_windows_h
#define mozilla_TimeStamp_windows_h

#include "mozilla/Types.h"

namespace mozilla {

/**
 * The [mt] unit:
 *
 * Many values are kept in ticks of the Performance Counter x 1000,
 * further just referred as [mt], meaning milli-ticks.
 *
 * This is needed to preserve maximum precision of the performance frequency
 * representation.  GetTickCount64 values in milliseconds are multiplied with
 * frequency per second.  Therefore we need to multiply QPC value by 1000 to
 * have the same units to allow simple arithmentic with both QPC and GTC.
 */
#define ms2mt(x) ((x)*mozilla::GetQueryPerformanceFrequencyPerSec())
#define mt2ms(x) ((x) / mozilla::GetQueryPerformanceFrequencyPerSec())
#define mt2ms_f(x) (double(x) / mozilla::GetQueryPerformanceFrequencyPerSec())

MFBT_API uint64_t GetQueryPerformanceFrequencyPerSec();

class TimeStamp;
class TimeStampValue;

TimeStampValue NowInternal(bool aHighResolution);

class TimeStampValue {
  friend TimeStampValue NowInternal(bool);
  friend bool IsCanonicalTimeStamp(TimeStampValue);
  friend struct IPC::ParamTraits<mozilla::TimeStampValue>;
  friend class TimeStamp;
  friend class Fuzzyfox;

  // Both QPC and GTC are kept in [mt] units.
  uint64_t mGTC;
  uint64_t mQPC;

  bool mUsedCanonicalNow;
  bool mIsNull;
  bool mHasQPC;

  MFBT_API TimeStampValue(uint64_t aGTC, uint64_t aQPC, bool aHasQPC,
                          bool aUsedCanonicalNow);

  MFBT_API uint64_t CheckQPC(const TimeStampValue& aOther) const;

  constexpr MOZ_IMPLICIT TimeStampValue()
      : mGTC(0),
        mQPC(0),
        mUsedCanonicalNow(false),
        mIsNull(true),
        mHasQPC(false) {}

 public:
  MFBT_API uint64_t operator-(const TimeStampValue& aOther) const;

  TimeStampValue operator+(const int64_t aOther) const {
    return TimeStampValue(mGTC + aOther, mQPC + aOther, mHasQPC,
                          mUsedCanonicalNow);
  }
  TimeStampValue operator-(const int64_t aOther) const {
    return TimeStampValue(mGTC - aOther, mQPC - aOther, mHasQPC,
                          mUsedCanonicalNow);
  }
  MFBT_API TimeStampValue& operator+=(const int64_t aOther);
  MFBT_API TimeStampValue& operator-=(const int64_t aOther);

  bool operator<(const TimeStampValue& aOther) const {
    return int64_t(*this - aOther) < 0;
  }
  bool operator>(const TimeStampValue& aOther) const {
    return int64_t(*this - aOther) > 0;
  }
  bool operator<=(const TimeStampValue& aOther) const {
    return int64_t(*this - aOther) <= 0;
  }
  bool operator>=(const TimeStampValue& aOther) const {
    return int64_t(*this - aOther) >= 0;
  }
  bool operator==(const TimeStampValue& aOther) const {
    return int64_t(*this - aOther) == 0;
  }
  bool operator!=(const TimeStampValue& aOther) const {
    return int64_t(*this - aOther) != 0;
  }
  bool UsedCanonicalNow() const { return mUsedCanonicalNow; }
  void SetCanonicalNow() { mUsedCanonicalNow = true; }
  bool IsNull() const { return mIsNull; }
};

}  // namespace mozilla

#endif /* mozilla_TimeStamp_h */
