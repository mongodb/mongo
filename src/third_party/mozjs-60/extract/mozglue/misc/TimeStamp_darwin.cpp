/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//
// Implement TimeStamp::Now() with mach_absolute_time
//
// The "tick" unit for mach_absolute_time is defined using mach_timebase_info() which
// gives a conversion ratio to nanoseconds. For more information see Apple's QA1398.
//
// This code is inspired by Chromium's time_mac.cc. The biggest
// differences are that we explicitly initialize using
// TimeStamp::Initialize() instead of lazily in Now() and that
// we store the time value in ticks and convert when needed instead
// of storing the time value in nanoseconds.

#include <mach/mach_time.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "mozilla/TimeStamp.h"

// Estimate of the smallest duration of time we can measure.
static uint64_t sResolution;
static uint64_t sResolutionSigDigs;

static const uint64_t kNsPerMs   =    1000000;
static const uint64_t kUsPerSec  =    1000000;
static const double kNsPerMsd    =    1000000.0;
static const double kNsPerSecd   = 1000000000.0;

static bool gInitialized = false;
static double sNsPerTick;

static uint64_t
ClockTime()
{
  // mach_absolute_time is it when it comes to ticks on the Mac.  Other calls
  // with less precision (such as TickCount) just call through to
  // mach_absolute_time.
  //
  // At the time of writing mach_absolute_time returns the number of nanoseconds
  // since boot. This won't overflow 64bits for 500+ years so we aren't going
  // to worry about that possiblity
  return mach_absolute_time();
}

static uint64_t
ClockResolutionNs()
{
  uint64_t start = ClockTime();
  uint64_t end = ClockTime();
  uint64_t minres = (end - start);

  // 10 total trials is arbitrary: what we're trying to avoid by
  // looping is getting unlucky and being interrupted by a context
  // switch or signal, or being bitten by paging/cache effects
  for (int i = 0; i < 9; ++i) {
    start = ClockTime();
    end = ClockTime();

    uint64_t candidate = (start - end);
    if (candidate < minres) {
      minres = candidate;
    }
  }

  if (0 == minres) {
    // measurable resolution is either incredibly low, ~1ns, or very
    // high.  fall back on NSPR's resolution assumption
    minres = 1 * kNsPerMs;
  }

  return minres;
}

namespace mozilla {

double
BaseTimeDurationPlatformUtils::ToSeconds(int64_t aTicks)
{
  MOZ_ASSERT(gInitialized, "calling TimeDuration too early");
  return (aTicks * sNsPerTick) / kNsPerSecd;
}

double
BaseTimeDurationPlatformUtils::ToSecondsSigDigits(int64_t aTicks)
{
  MOZ_ASSERT(gInitialized, "calling TimeDuration too early");
  // don't report a value < mResolution ...
  int64_t valueSigDigs = sResolution * (aTicks / sResolution);
  // and chop off insignificant digits
  valueSigDigs = sResolutionSigDigs * (valueSigDigs / sResolutionSigDigs);
  return (valueSigDigs * sNsPerTick) / kNsPerSecd;
}

int64_t
BaseTimeDurationPlatformUtils::TicksFromMilliseconds(double aMilliseconds)
{
  MOZ_ASSERT(gInitialized, "calling TimeDuration too early");
  double result = (aMilliseconds * kNsPerMsd) / sNsPerTick;
  if (result > INT64_MAX) {
    return INT64_MAX;
  } else if (result < INT64_MIN) {
    return INT64_MIN;
  }

  return result;
}

int64_t
BaseTimeDurationPlatformUtils::ResolutionInTicks()
{
  MOZ_ASSERT(gInitialized, "calling TimeDuration too early");
  return static_cast<int64_t>(sResolution);
}

void
TimeStamp::Startup()
{
  if (gInitialized) {
    return;
  }

  mach_timebase_info_data_t timebaseInfo;
  // Apple's QA1398 suggests that the output from mach_timebase_info
  // will not change while a program is running, so it should be safe
  // to cache the result.
  kern_return_t kr = mach_timebase_info(&timebaseInfo);
  if (kr != KERN_SUCCESS) {
    MOZ_RELEASE_ASSERT(false, "mach_timebase_info failed");
  }

  sNsPerTick = double(timebaseInfo.numer) / timebaseInfo.denom;

  sResolution = ClockResolutionNs();

  // find the number of significant digits in sResolution, for the
  // sake of ToSecondsSigDigits()
  for (sResolutionSigDigs = 1;
       !(sResolutionSigDigs == sResolution ||
         10 * sResolutionSigDigs > sResolution);
       sResolutionSigDigs *= 10);

  gInitialized = true;

  return;
}

void
TimeStamp::Shutdown()
{
}

TimeStamp
TimeStamp::Now(bool aHighResolution)
{
  return TimeStamp(ClockTime());
}

// Computes and returns the process uptime in microseconds.
// Returns 0 if an error was encountered.

uint64_t
TimeStamp::ComputeProcessUptime()
{
  struct timeval tv;
  int rv = gettimeofday(&tv, nullptr);

  if (rv == -1) {
    return 0;
  }

  int mib[] = {
    CTL_KERN,
    KERN_PROC,
    KERN_PROC_PID,
    getpid(),
  };
  u_int mibLen = sizeof(mib) / sizeof(mib[0]);

  struct kinfo_proc proc;
  size_t bufferSize = sizeof(proc);
  rv = sysctl(mib, mibLen, &proc, &bufferSize, nullptr, 0);

  if (rv == -1) {
    return 0;
  }

  uint64_t startTime =
    ((uint64_t)proc.kp_proc.p_un.__p_starttime.tv_sec * kUsPerSec) +
    proc.kp_proc.p_un.__p_starttime.tv_usec;
  uint64_t now = (tv.tv_sec * kUsPerSec) + tv.tv_usec;

  if (startTime > now) {
    return 0;
  }

  return now - startTime;
}

} // namespace mozilla
