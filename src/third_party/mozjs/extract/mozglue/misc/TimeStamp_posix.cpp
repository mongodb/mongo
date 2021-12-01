/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//
// Implement TimeStamp::Now() with POSIX clocks.
//
// The "tick" unit for POSIX clocks is simply a nanosecond, as this is
// the smallest unit of time representable by struct timespec.  That
// doesn't mean that a nanosecond is the resolution of TimeDurations
// obtained with this API; see TimeDuration::Resolution;
//

#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#if defined(__DragonFly__) || defined(__FreeBSD__) \
    || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

#if defined(__DragonFly__) || defined(__FreeBSD__)
#include <sys/user.h>
#endif

#if defined(__NetBSD__)
#undef KERN_PROC
#define KERN_PROC KERN_PROC2
#define KINFO_PROC struct kinfo_proc2
#else
#define KINFO_PROC struct kinfo_proc
#endif

#if defined(__DragonFly__)
#define KP_START_SEC kp_start.tv_sec
#define KP_START_USEC kp_start.tv_usec
#elif defined(__FreeBSD__)
#define KP_START_SEC ki_start.tv_sec
#define KP_START_USEC ki_start.tv_usec
#else
#define KP_START_SEC p_ustart_sec
#define KP_START_USEC p_ustart_usec
#endif

#include "mozilla/Sprintf.h"
#include "mozilla/TimeStamp.h"
#include <pthread.h>

// Estimate of the smallest duration of time we can measure.
static uint64_t sResolution;
static uint64_t sResolutionSigDigs;

static const uint16_t kNsPerUs   =       1000;
static const uint64_t kNsPerMs   =    1000000;
static const uint64_t kNsPerSec  = 1000000000;
static const double kNsPerMsd    =    1000000.0;
static const double kNsPerSecd   = 1000000000.0;

static uint64_t
TimespecToNs(const struct timespec& aTs)
{
  uint64_t baseNs = uint64_t(aTs.tv_sec) * kNsPerSec;
  return baseNs + uint64_t(aTs.tv_nsec);
}

static uint64_t
ClockTimeNs()
{
  struct timespec ts;
  // this can't fail: we know &ts is valid, and TimeStamp::Startup()
  // checks that CLOCK_MONOTONIC is supported (and aborts if not)
  clock_gettime(CLOCK_MONOTONIC, &ts);

  // tv_sec is defined to be relative to an arbitrary point in time,
  // but it would be madness for that point in time to be earlier than
  // the Epoch.  So we can safely assume that even if time_t is 32
  // bits, tv_sec won't overflow while the browser is open.  Revisit
  // this argument if we're still building with 32-bit time_t around
  // the year 2037.
  return TimespecToNs(ts);
}

static uint64_t
ClockResolutionNs()
{
  // NB: why not rely on clock_getres()?  Two reasons: (i) it might
  // lie, and (ii) it might return an "ideal" resolution that while
  // theoretically true, could never be measured in practice.  Since
  // clock_gettime() likely involves a system call on your platform,
  // the "actual" timing resolution shouldn't be lower than syscall
  // overhead.

  uint64_t start = ClockTimeNs();
  uint64_t end = ClockTimeNs();
  uint64_t minres = (end - start);

  // 10 total trials is arbitrary: what we're trying to avoid by
  // looping is getting unlucky and being interrupted by a context
  // switch or signal, or being bitten by paging/cache effects
  for (int i = 0; i < 9; ++i) {
    start = ClockTimeNs();
    end = ClockTimeNs();

    uint64_t candidate = (start - end);
    if (candidate < minres) {
      minres = candidate;
    }
  }

  if (0 == minres) {
    // measurable resolution is either incredibly low, ~1ns, or very
    // high.  fall back on clock_getres()
    struct timespec ts;
    if (0 == clock_getres(CLOCK_MONOTONIC, &ts)) {
      minres = TimespecToNs(ts);
    }
  }

  if (0 == minres) {
    // clock_getres probably failed.  fall back on NSPR's resolution
    // assumption
    minres = 1 * kNsPerMs;
  }

  return minres;
}

namespace mozilla {

double
BaseTimeDurationPlatformUtils::ToSeconds(int64_t aTicks)
{
  return double(aTicks) / kNsPerSecd;
}

double
BaseTimeDurationPlatformUtils::ToSecondsSigDigits(int64_t aTicks)
{
  // don't report a value < mResolution ...
  int64_t valueSigDigs = sResolution * (aTicks / sResolution);
  // and chop off insignificant digits
  valueSigDigs = sResolutionSigDigs * (valueSigDigs / sResolutionSigDigs);
  return double(valueSigDigs) / kNsPerSecd;
}

int64_t
BaseTimeDurationPlatformUtils::TicksFromMilliseconds(double aMilliseconds)
{
  double result = aMilliseconds * kNsPerMsd;
  if (result > INT64_MAX) {
    return INT64_MAX;
  }
  if (result < INT64_MIN) {
    return INT64_MIN;
  }

  return result;
}

int64_t
BaseTimeDurationPlatformUtils::ResolutionInTicks()
{
  return static_cast<int64_t>(sResolution);
}

static bool gInitialized = false;

void
TimeStamp::Startup()
{
  if (gInitialized) {
    return;
  }

  struct timespec dummy;
  if (clock_gettime(CLOCK_MONOTONIC, &dummy) != 0) {
    MOZ_CRASH("CLOCK_MONOTONIC is absent!");
  }

  sResolution = ClockResolutionNs();

  // find the number of significant digits in sResolution, for the
  // sake of ToSecondsSigDigits()
  for (sResolutionSigDigs = 1;
       !(sResolutionSigDigs == sResolution ||
         10 * sResolutionSigDigs > sResolution);
       sResolutionSigDigs *= 10);

  gInitialized = true;
}

void
TimeStamp::Shutdown()
{
}

TimeStamp
TimeStamp::Now(bool aHighResolution)
{
  return TimeStamp(ClockTimeNs());
}

#if defined(XP_LINUX) || defined(ANDROID)

// Calculates the amount of jiffies that have elapsed since boot and up to the
// starttime value of a specific process as found in its /proc/*/stat file.
// Returns 0 if an error occurred.

static uint64_t
JiffiesSinceBoot(const char* aFile)
{
  char stat[512];

  FILE* f = fopen(aFile, "r");
  if (!f) {
    return 0;
  }

  int n = fread(&stat, 1, sizeof(stat) - 1, f);

  fclose(f);

  if (n <= 0) {
    return 0;
  }

  stat[n] = 0;

  long long unsigned startTime = 0; // instead of uint64_t to keep GCC quiet
  char* s = strrchr(stat, ')');

  if (!s) {
    return 0;
  }

  int rv = sscanf(s + 2,
                  "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u "
                  "%*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                  &startTime);

  if (rv != 1 || !startTime) {
    return 0;
  }

  return startTime;
}

// Computes the interval that has elapsed between the thread creation and the
// process creation by comparing the starttime fields in the respective
// /proc/*/stat files. The resulting value will be a good approximation of the
// process uptime. This value will be stored at the address pointed by aTime;
// if an error occurred 0 will be stored instead.

static void*
ComputeProcessUptimeThread(void* aTime)
{
  uint64_t* uptime = static_cast<uint64_t*>(aTime);
  long hz = sysconf(_SC_CLK_TCK);

  *uptime = 0;

  if (!hz) {
    return nullptr;
  }

  char threadStat[40];
  SprintfLiteral(threadStat, "/proc/self/task/%d/stat", (pid_t)syscall(__NR_gettid));

  uint64_t threadJiffies = JiffiesSinceBoot(threadStat);
  uint64_t selfJiffies = JiffiesSinceBoot("/proc/self/stat");

  if (!threadJiffies || !selfJiffies) {
    return nullptr;
  }

  *uptime = ((threadJiffies - selfJiffies) * kNsPerSec) / hz;
  return nullptr;
}

// Computes and returns the process uptime in us on Linux & its derivatives.
// Returns 0 if an error was encountered.

uint64_t
TimeStamp::ComputeProcessUptime()
{
  uint64_t uptime = 0;
  pthread_t uptime_pthread;

  if (pthread_create(&uptime_pthread, nullptr, ComputeProcessUptimeThread, &uptime)) {
    MOZ_CRASH("Failed to create process uptime thread.");
    return 0;
  }

  pthread_join(uptime_pthread, NULL);

  return uptime / kNsPerUs;
}

#elif defined(__DragonFly__) || defined(__FreeBSD__) \
      || defined(__NetBSD__) || defined(__OpenBSD__)

// Computes and returns the process uptime in us on various BSD flavors.
// Returns 0 if an error was encountered.

uint64_t
TimeStamp::ComputeProcessUptime()
{
  struct timespec ts;
  int rv = clock_gettime(CLOCK_REALTIME, &ts);

  if (rv == -1) {
    return 0;
  }

  int mib[] = {
    CTL_KERN,
    KERN_PROC,
    KERN_PROC_PID,
    getpid(),
#if defined(__NetBSD__) || defined(__OpenBSD__)
    sizeof(KINFO_PROC),
    1,
#endif
  };
  u_int mibLen = sizeof(mib) / sizeof(mib[0]);

  KINFO_PROC proc;
  size_t bufferSize = sizeof(proc);
  rv = sysctl(mib, mibLen, &proc, &bufferSize, nullptr, 0);

  if (rv == -1) {
    return 0;
  }

  uint64_t startTime = ((uint64_t)proc.KP_START_SEC * kNsPerSec) +
    (proc.KP_START_USEC * kNsPerUs);
  uint64_t now = ((uint64_t)ts.tv_sec * kNsPerSec) + ts.tv_nsec;

  if (startTime > now) {
    return 0;
  }

  return (now - startTime) / kNsPerUs;
}

#else

uint64_t
TimeStamp::ComputeProcessUptime()
{
  return 0;
}

#endif

} // namespace mozilla
