/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Implementation of the OS-independent methods of the TimeStamp class
 */

#include "mozilla/Atomics.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Uptime.h"
#include <stdio.h>
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
  };

  ~TimeStampInitialization() { TimeStamp::Shutdown(); };
};

static bool sFuzzyfoxEnabled;

/* static */
bool TimeStamp::GetFuzzyfoxEnabled() { return sFuzzyfoxEnabled; }

/* static */
void TimeStamp::SetFuzzyfoxEnabled(bool aValue) { sFuzzyfoxEnabled = aValue; }

// These variables store the frozen time (as a TimeStamp) for FuzzyFox that
// will be reported if FuzzyFox is enabled.
// We overload the top bit of sCanonicalNow and sCanonicalGTC to
// indicate if a Timestamp is a fuzzed timestamp (bit set) or not
// (bit unset).
#ifdef XP_WIN
static Atomic<uint64_t> sCanonicalGTC;
static Atomic<uint64_t> sCanonicalQPC;
static Atomic<bool> sCanonicalHasQPC;
#else
static Atomic<uint64_t> sCanonicalNowTimeStamp;
#endif
static Atomic<int64_t> sCanonicalNowTime;
// This variable stores the frozen time (as ms since the epoch) for FuzzyFox
// to report if FuzzyFox is enabled.
static TimeStampInitialization sInitOnce;

MFBT_API TimeStamp TimeStamp::ProcessCreation(bool* aIsInconsistent) {
  if (aIsInconsistent) {
    *aIsInconsistent = false;
  }

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

      ts = now - TimeDuration::FromMicroseconds(uptime);

      if ((ts > sInitOnce.mFirstTimeStamp) || (uptime == 0)) {
        /* If the process creation timestamp was inconsistent replace it with
         * the first one instead and notify that a telemetry error was
         * detected. */
        if (aIsInconsistent) {
          *aIsInconsistent = true;
        }
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

MFBT_API TimeStamp TimeStamp::NowFuzzy(TimeStampValue aValue) {
#ifdef XP_WIN
  TimeStampValue canonicalNow =
      TimeStampValue(sCanonicalGTC, sCanonicalQPC, sCanonicalHasQPC, true);
#else
  TimeStampValue canonicalNow = TimeStampValue(sCanonicalNowTimeStamp);
#endif

  if (TimeStamp::GetFuzzyfoxEnabled()) {
    if (MOZ_LIKELY(!canonicalNow.IsNull())) {
      return TimeStamp(canonicalNow);
    }
  }
  // When we disable Fuzzyfox, time may goes backwards, so we need to make sure
  // we don't do that.
  else if (MOZ_UNLIKELY(canonicalNow > aValue)) {
    return TimeStamp(canonicalNow);
  }

  return TimeStamp(aValue);
}

MFBT_API void TimeStamp::UpdateFuzzyTimeStamp(TimeStamp aValue) {
#ifdef XP_WIN
  sCanonicalGTC = aValue.mValue.mGTC;
  sCanonicalQPC = aValue.mValue.mQPC;
  sCanonicalHasQPC = aValue.mValue.mHasQPC;
#else
  sCanonicalNowTimeStamp = aValue.mValue.mTimeStamp;
#endif
}

MFBT_API int64_t TimeStamp::NowFuzzyTime() { return sCanonicalNowTime; }

MFBT_API void TimeStamp::UpdateFuzzyTime(int64_t aValue) {
  sCanonicalNowTime = aValue;
}

}  // namespace mozilla
