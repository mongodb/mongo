/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AwakeTimeStamp.h"

#ifdef XP_WIN
#  include <windows.h>
#endif

#include "mozilla/Assertions.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

namespace mozilla {

static constexpr uint64_t kUSperS = 1000000;
static constexpr uint64_t kUSperMS = 1000;
#ifndef XP_WIN
static constexpr uint64_t kNSperUS = 1000;
#endif

double AwakeTimeDuration::ToSeconds() const {
  return static_cast<double>(mValueUs) / kUSperS;
}
double AwakeTimeDuration::ToMilliseconds() const {
  return static_cast<double>(mValueUs) / kUSperMS;
}
double AwakeTimeDuration::ToMicroseconds() const {
  return static_cast<double>(mValueUs);
}

AwakeTimeDuration AwakeTimeStamp::operator-(
    AwakeTimeStamp const& aOther) const {
  return AwakeTimeDuration(mValueUs - aOther.mValueUs);
}

AwakeTimeStamp AwakeTimeStamp::operator+(
    const AwakeTimeDuration& aDuration) const {
  return AwakeTimeStamp(mValueUs + aDuration.mValueUs);
}

void AwakeTimeStamp::operator+=(const AwakeTimeDuration& aOther) {
  mValueUs += aOther.mValueUs;
}

void AwakeTimeStamp::operator-=(const AwakeTimeDuration& aOther) {
  MOZ_ASSERT(mValueUs >= aOther.mValueUs);
  mValueUs -= aOther.mValueUs;
}

// Apple things
#if defined(__APPLE__) && defined(__MACH__)
#  include <time.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <mach/mach_time.h>

AwakeTimeStamp AwakeTimeStamp::NowLoRes() {
  return AwakeTimeStamp(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / kNSperUS);
}

#elif defined(XP_WIN)

// Number of hundreds of nanoseconds in a microsecond
static constexpr uint64_t kHNSperUS = 10;

AwakeTimeStamp AwakeTimeStamp::NowLoRes() {
  ULONGLONG interrupt_time;
  DebugOnly<bool> rv = QueryUnbiasedInterruptTime(&interrupt_time);
  MOZ_ASSERT(rv);

  return AwakeTimeStamp(interrupt_time / kHNSperUS);
}

#else  // Linux and other POSIX but not macOS
#  include <time.h>

uint64_t TimespecToMicroseconds(struct timespec aTs) {
  return aTs.tv_sec * kUSperS + aTs.tv_nsec / kNSperUS;
}

AwakeTimeStamp AwakeTimeStamp::NowLoRes() {
  struct timespec ts = {0};
  DebugOnly<int> rv = clock_gettime(CLOCK_MONOTONIC, &ts);
  MOZ_ASSERT(!rv);
  return AwakeTimeStamp(TimespecToMicroseconds(ts));
}

#endif

};  // namespace mozilla
