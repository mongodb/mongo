/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implementation of the OS-independent methods of the TimeStamp class
 */

#include "mozilla/TimeStamp.h"
#include "mozilla/Uptime.h"
#include <string.h>
#include <stdlib.h>

namespace mozilla {

/**
 * Wrapper class used to initialize static data used by the TimeStamp class
 */
struct TimeStampInitialization {
  /**
   * First timestamp taken when the class static initializers are run. This
   * timestamp is used to sanitize timestamps coming from different sources.
   */
  TimeStamp mFirstTimeStamp;

  /**
   * Timestamp representing the time when the process was created. This field
   * is populated lazily the first time this information is required and is
   * replaced every time the process is restarted.
   */
  TimeStamp mProcessCreation;

  TimeStampInitialization() {
    TimeStamp::Startup();
    mFirstTimeStamp = TimeStamp::Now();
    // On Windows < 10, initializing the uptime requires `mFirstTimeStamp` to be
    // valid.
    mozilla::InitializeUptime();
  }

  ~TimeStampInitialization() { TimeStamp::Shutdown(); }
};

MOZ_RUNINIT static TimeStampInitialization sInitOnce;

MFBT_API TimeStamp TimeStamp::ProcessCreation() {
  if (sInitOnce.mProcessCreation.IsNull()) {
    char* mozAppRestart = getenv("MOZ_APP_RESTART");
    TimeStamp ts;

    /* When calling PR_SetEnv() with an empty value the existing variable may
     * be unset or set to the empty string depending on the underlying platform
     * thus we have to check if the variable is present and not empty. */
    if (mozAppRestart && (strcmp(mozAppRestart, "") != 0)) {
      /* Firefox was restarted, use the first time-stamp we've taken as the new
       * process startup time. */
      ts = sInitOnce.mFirstTimeStamp;
    } else {
      TimeStamp now = Now();
      uint64_t uptime = ComputeProcessUptime();

      ts = now - TimeDuration::FromMicroseconds(static_cast<double>(uptime));

      if ((ts > sInitOnce.mFirstTimeStamp) || (uptime == 0)) {
        ts = sInitOnce.mFirstTimeStamp;
      }
    }

    sInitOnce.mProcessCreation = ts;
  }

  return sInitOnce.mProcessCreation;
}

void TimeStamp::RecordProcessRestart() {
  sInitOnce.mProcessCreation = TimeStamp();
}

MFBT_API TimeStamp TimeStamp::FirstTimeStamp() {
  return sInitOnce.mFirstTimeStamp;
}

class TimeStampTests {
  // Check that nullity is set/not set correctly.
  static_assert(TimeStamp{TimeStampValue{0}}.IsNull());
  static_assert(!TimeStamp{TimeStampValue{1}}.IsNull());

  // Check that some very basic comparisons work correctly.
  static constexpr uint64_t sMidTime = (uint64_t)1 << 63;
  static_assert(TimeStampValue{sMidTime + 5} > TimeStampValue{sMidTime - 5});
  static_assert(TimeStampValue{sMidTime + 5} >= TimeStampValue{sMidTime - 5});
  static_assert(TimeStampValue{sMidTime - 5} < TimeStampValue{sMidTime + 5});
  static_assert(TimeStampValue{sMidTime - 5} <= TimeStampValue{sMidTime + 5});
  static_assert(TimeStampValue{sMidTime} == TimeStampValue{sMidTime});
  static_assert(TimeStampValue{sMidTime} >= TimeStampValue{sMidTime});
  static_assert(TimeStampValue{sMidTime} <= TimeStampValue{sMidTime});
  static_assert(TimeStampValue{sMidTime - 5} != TimeStampValue{sMidTime + 5});

  // Check that comparisons involving very large and very small TimeStampValue's
  // work correctly. This may seem excessive, but these asserts would have
  // failed in the past due to a comparison such as "a > b" being implemented as
  // "<cast to signed 64-bit value>(a - b) > 0". When a-b didn't fit into a
  // signed 64-bit value, this would have given an incorrect result.
  static_assert(TimeStampValue{UINT64_MAX} > TimeStampValue{1});
  static_assert(TimeStampValue{1} < TimeStampValue{UINT64_MAX});

  // NOTE/TODO: It would be nice to add some additional tests here that involve
  // arithmetic between TimeStamps and TimeDurations (and verifying some of the
  // special behaviors in some cases such as not wrapping around below zero) but
  // that is not possible right now because those operators are not constexpr.
};

}  // namespace mozilla
