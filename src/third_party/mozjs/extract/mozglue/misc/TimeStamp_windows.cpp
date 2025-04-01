/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Implement TimeStamp::Now() with QueryPerformanceCounter() controlled with
// values of GetTickCount64().

#include "mozilla/DynamicallyLinkedFunctionPtr.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/Uptime.h"

#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>
#include <windows.h>

// To enable logging define to your favorite logging API
#define LOG(x)

class AutoCriticalSection {
 public:
  explicit AutoCriticalSection(LPCRITICAL_SECTION aSection)
      : mSection(aSection) {
    ::EnterCriticalSection(mSection);
  }
  ~AutoCriticalSection() { ::LeaveCriticalSection(mSection); }

 private:
  LPCRITICAL_SECTION mSection;
};

// Estimate of the smallest duration of time we can measure.
static volatile ULONGLONG sResolution;
static volatile ULONGLONG sResolutionSigDigs;
static const double kNsPerSecd = 1000000000.0;
static const LONGLONG kNsPerMillisec = 1000000;

// ----------------------------------------------------------------------------
// Global constants
// ----------------------------------------------------------------------------

// Tolerance to failures settings.
//
// What is the interval we want to have failure free.
// in [ms]
static const uint32_t kFailureFreeInterval = 5000;
// How many failures we are willing to tolerate in the interval.
static const uint32_t kMaxFailuresPerInterval = 4;
// What is the threshold to treat fluctuations as actual failures.
// in [ms]
static const uint32_t kFailureThreshold = 50;

// If we are not able to get the value of GTC time increment, use this value
// which is the most usual increment.
static const DWORD kDefaultTimeIncrement = 156001;

// ----------------------------------------------------------------------------
// Global variables, not changing at runtime
// ----------------------------------------------------------------------------

// Result of QueryPerformanceFrequency
// We use default of 1 for the case we can't use QueryPerformanceCounter
// to make mt/ms conversions work despite that.
static uint64_t sFrequencyPerSec = 1;

namespace mozilla {

MFBT_API uint64_t GetQueryPerformanceFrequencyPerSec() {
  return sFrequencyPerSec;
}

}  // namespace mozilla

// How much we are tolerant to GTC occasional loose of resoltion.
// This number says how many multiples of the minimal GTC resolution
// detected on the system are acceptable.  This number is empirical.
static const LONGLONG kGTCTickLeapTolerance = 4;

// Base tolerance (more: "inability of detection" range) threshold is calculated
// dynamically, and kept in sGTCResolutionThreshold.
//
// Schematically, QPC worked "100%" correctly if ((GTC_now - GTC_epoch) -
// (QPC_now - QPC_epoch)) was in  [-sGTCResolutionThreshold,
// sGTCResolutionThreshold] interval every time we'd compared two time stamps.
// If not, then we check the overflow behind this basic threshold
// is in kFailureThreshold.  If not, we condider it as a QPC failure.  If too
// many failures in short time are detected, QPC is considered faulty and
// disabled.
//
// Kept in [mt]
static LONGLONG sGTCResolutionThreshold;

// If QPC is found faulty for two stamps in this interval, we engage
// the fault detection algorithm.  For duration larger then this limit
// we bypass using durations calculated from QPC when jitter is detected,
// but don't touch the sUseQPC flag.
//
// Value is in [ms].
static const uint32_t kHardFailureLimit = 2000;
// Conversion to [mt]
static LONGLONG sHardFailureLimit;

// Conversion of kFailureFreeInterval and kFailureThreshold to [mt]
static LONGLONG sFailureFreeInterval;
static LONGLONG sFailureThreshold;

// ----------------------------------------------------------------------------
// Systemm status flags
// ----------------------------------------------------------------------------

// Flag for stable TSC that indicates platform where QPC is stable.
static bool sHasStableTSC = false;

// ----------------------------------------------------------------------------
// Global state variables, changing at runtime
// ----------------------------------------------------------------------------

// Initially true, set to false when QPC is found unstable and never
// returns back to true since that time.
static bool volatile sUseQPC = true;

// ----------------------------------------------------------------------------
// Global lock
// ----------------------------------------------------------------------------

// Thread spin count before entering the full wait state for sTimeStampLock.
// Inspired by Rob Arnold's work on PRMJ_Now().
static const DWORD kLockSpinCount = 4096;

// Common mutex (thanks the relative complexity of the logic, this is better
// then using CMPXCHG8B.)
// It is protecting the globals bellow.
static CRITICAL_SECTION sTimeStampLock;

// ----------------------------------------------------------------------------
// Global lock protected variables
// ----------------------------------------------------------------------------

// Timestamp in future until QPC must behave correctly.
// Set to now + kFailureFreeInterval on first QPC failure detection.
// Set to now + E * kFailureFreeInterval on following errors,
//   where E is number of errors detected during last kFailureFreeInterval
//   milliseconds, calculated simply as:
//   E = (sFaultIntoleranceCheckpoint - now) / kFailureFreeInterval + 1.
// When E > kMaxFailuresPerInterval -> disable QPC.
//
// Kept in [mt]
static ULONGLONG sFaultIntoleranceCheckpoint = 0;

namespace mozilla {

// Result is in [mt]
static inline ULONGLONG PerformanceCounter() {
  LARGE_INTEGER pc;
  ::QueryPerformanceCounter(&pc);

  // QueryPerformanceCounter may slightly jitter (not be 100% monotonic.)
  // This is a simple go-backward protection for such a faulty hardware.
  AutoCriticalSection lock(&sTimeStampLock);

  static decltype(LARGE_INTEGER::QuadPart) last;
  if (last > pc.QuadPart) {
    return last * 1000ULL;
  }
  last = pc.QuadPart;
  return pc.QuadPart * 1000ULL;
}

static void InitThresholds() {
  DWORD timeAdjustment = 0, timeIncrement = 0;
  BOOL timeAdjustmentDisabled;
  GetSystemTimeAdjustment(&timeAdjustment, &timeIncrement,
                          &timeAdjustmentDisabled);

  LOG(("TimeStamp: timeIncrement=%d [100ns]", timeIncrement));

  if (!timeIncrement) {
    timeIncrement = kDefaultTimeIncrement;
  }

  // Ceiling to a millisecond
  // Example values: 156001, 210000
  DWORD timeIncrementCeil = timeIncrement;
  // Don't want to round up if already rounded, values will be: 156000, 209999
  timeIncrementCeil -= 1;
  // Convert to ms, values will be: 15, 20
  timeIncrementCeil /= 10000;
  // Round up, values will be: 16, 21
  timeIncrementCeil += 1;
  // Convert back to 100ns, values will be: 160000, 210000
  timeIncrementCeil *= 10000;

  // How many milli-ticks has the interval rounded up
  LONGLONG ticksPerGetTickCountResolutionCeiling =
      (int64_t(timeIncrementCeil) * sFrequencyPerSec) / 10000LL;

  // GTC may jump by 32 (2*16) ms in two steps, therefor use the ceiling value.
  sGTCResolutionThreshold =
      LONGLONG(kGTCTickLeapTolerance * ticksPerGetTickCountResolutionCeiling);

  sHardFailureLimit = ms2mt(kHardFailureLimit);
  sFailureFreeInterval = ms2mt(kFailureFreeInterval);
  sFailureThreshold = ms2mt(kFailureThreshold);
}

static void InitResolution() {
  // 10 total trials is arbitrary: what we're trying to avoid by
  // looping is getting unlucky and being interrupted by a context
  // switch or signal, or being bitten by paging/cache effects

  ULONGLONG minres = ~0ULL;
  if (sUseQPC) {
    int loops = 10;
    do {
      ULONGLONG start = PerformanceCounter();
      ULONGLONG end = PerformanceCounter();

      ULONGLONG candidate = (end - start);
      if (candidate < minres) {
        minres = candidate;
      }
    } while (--loops && minres);

    if (0 == minres) {
      minres = 1;
    }
  } else {
    // GetTickCount has only ~16ms known resolution
    minres = ms2mt(16);
  }

  // Converting minres that is in [mt] to nanosecods, multiplicating
  // the argument to preserve resolution.
  ULONGLONG result = mt2ms(minres * kNsPerMillisec);
  if (0 == result) {
    result = 1;
  }

  sResolution = result;

  // find the number of significant digits in mResolution, for the
  // sake of ToSecondsSigDigits()
  ULONGLONG sigDigs;
  for (sigDigs = 1; !(sigDigs == result || 10 * sigDigs > result);
       sigDigs *= 10)
    ;

  sResolutionSigDigs = sigDigs;
}

// ----------------------------------------------------------------------------
// TimeStampValue implementation
// ----------------------------------------------------------------------------
MFBT_API TimeStampValue& TimeStampValue::operator+=(const int64_t aOther) {
  mGTC += aOther;
  mQPC += aOther;
  return *this;
}

MFBT_API TimeStampValue& TimeStampValue::operator-=(const int64_t aOther) {
  mGTC -= aOther;
  mQPC -= aOther;
  return *this;
}

// If the duration is less then two seconds, perform check of QPC stability
// by comparing both GTC and QPC calculated durations of this and aOther.
MFBT_API uint64_t TimeStampValue::CheckQPC(const TimeStampValue& aOther) const {
  uint64_t deltaGTC = mGTC - aOther.mGTC;

  if (!mHasQPC || !aOther.mHasQPC) {  // Both not holding QPC
    return deltaGTC;
  }

  uint64_t deltaQPC = mQPC - aOther.mQPC;

  if (sHasStableTSC) {  // For stable TSC there is no need to check
    return deltaQPC;
  }

  // Check QPC is sane before using it.
  int64_t diff = DeprecatedAbs(int64_t(deltaQPC) - int64_t(deltaGTC));
  if (diff <= sGTCResolutionThreshold) {
    return deltaQPC;
  }

  // Treat absolutely for calibration purposes
  int64_t duration = DeprecatedAbs(int64_t(deltaGTC));
  int64_t overflow = diff - sGTCResolutionThreshold;

  LOG(("TimeStamp: QPC check after %llums with overflow %1.4fms",
       mt2ms(duration), mt2ms_f(overflow)));

  if (overflow <= sFailureThreshold) {  // We are in the limit, let go.
    return deltaQPC;
  }

  // QPC deviates, don't use it, since now this method may only return deltaGTC.

  if (!sUseQPC) {  // QPC already disabled, no need to run the fault tolerance
                   // algorithm.
    return deltaGTC;
  }

  LOG(("TimeStamp: QPC jittered over failure threshold"));

  if (duration < sHardFailureLimit) {
    // Interval between the two time stamps is very short, consider
    // QPC as unstable and record a failure.
    uint64_t now = ms2mt(GetTickCount64());

    AutoCriticalSection lock(&sTimeStampLock);

    if (sFaultIntoleranceCheckpoint && sFaultIntoleranceCheckpoint > now) {
      // There's already been an error in the last fault intollerant interval.
      // Time since now to the checkpoint actually holds information on how many
      // failures there were in the failure free interval we have defined.
      uint64_t failureCount =
          (sFaultIntoleranceCheckpoint - now + sFailureFreeInterval - 1) /
          sFailureFreeInterval;
      if (failureCount > kMaxFailuresPerInterval) {
        sUseQPC = false;
        LOG(("TimeStamp: QPC disabled"));
      } else {
        // Move the fault intolerance checkpoint more to the future, prolong it
        // to reflect the number of detected failures.
        ++failureCount;
        sFaultIntoleranceCheckpoint = now + failureCount * sFailureFreeInterval;
        LOG(("TimeStamp: recording %dth QPC failure", failureCount));
      }
    } else {
      // Setup fault intolerance checkpoint in the future for first detected
      // error.
      sFaultIntoleranceCheckpoint = now + sFailureFreeInterval;
      LOG(("TimeStamp: recording 1st QPC failure"));
    }
  }

  return deltaGTC;
}

MFBT_API uint64_t
TimeStampValue::operator-(const TimeStampValue& aOther) const {
  if (IsNull() && aOther.IsNull()) {
    return uint64_t(0);
  }

  return CheckQPC(aOther);
}

class TimeStampValueTests {
  // Check that nullity is set/not set correctly.
  static_assert(TimeStampValue{0}.IsNull());
  static_assert(!TimeStampValue{1}.IsNull());

  // Check that we ignore GTC when both TimeStampValues have QPC. (In each of
  // these tests, looking at GTC would give a different result.)
  static_assert(TimeStampValue{1, 2, true} < TimeStampValue{1, 3, true});
  static_assert(!(TimeStampValue{1, 2, true} == TimeStampValue{1, 3, true}));

  static_assert(TimeStampValue{2, 2, true} < TimeStampValue{1, 3, true});
  static_assert(TimeStampValue{2, 2, true} <= TimeStampValue{1, 3, true});
  static_assert(!(TimeStampValue{2, 2, true} > TimeStampValue{1, 3, true}));

  static_assert(TimeStampValue{1, 3, true} > TimeStampValue{1, 2, true});
  static_assert(!(TimeStampValue{1, 3, true} == TimeStampValue{1, 2, true}));

  static_assert(TimeStampValue{1, 3, true} > TimeStampValue{2, 2, true});
  static_assert(TimeStampValue{1, 3, true} >= TimeStampValue{2, 2, true});
  static_assert(!(TimeStampValue{1, 3, true} < TimeStampValue{2, 2, true}));

  static_assert(TimeStampValue{1, 3, true} == TimeStampValue{2, 3, true});
  static_assert(!(TimeStampValue{1, 3, true} < TimeStampValue{2, 3, true}));

  static_assert(TimeStampValue{1, 2, true} != TimeStampValue{1, 3, true});
  static_assert(!(TimeStampValue{1, 2, true} == TimeStampValue{1, 3, true}));

  // Check that, if either TimeStampValue doesn't have QPC, we only look at the
  // GTC values. These are the same cases as above, except that we accept the
  // opposite results because we turn off QPC on one or both of the
  // TimeStampValue's.
  static_assert(TimeStampValue{1, 2, false} == TimeStampValue{1, 3, true});
  static_assert(TimeStampValue{1, 2, true} == TimeStampValue{1, 3, false});
  static_assert(TimeStampValue{1, 2, false} == TimeStampValue{1, 3, false});

  static_assert(TimeStampValue{2, 2, false} > TimeStampValue{1, 3, true});
  static_assert(TimeStampValue{2, 2, true} > TimeStampValue{1, 3, false});
  static_assert(TimeStampValue{2, 2, false} > TimeStampValue{1, 3, false});

  static_assert(TimeStampValue{1, 3, false} == TimeStampValue{1, 2, true});
  static_assert(TimeStampValue{1, 3, true} == TimeStampValue{1, 2, false});
  static_assert(TimeStampValue{1, 3, false} == TimeStampValue{1, 2, false});

  static_assert(TimeStampValue{1, 3, false} < TimeStampValue{2, 2, true});
  static_assert(TimeStampValue{1, 3, true} < TimeStampValue{2, 2, false});
  static_assert(TimeStampValue{1, 3, false} < TimeStampValue{2, 2, false});

  static_assert(TimeStampValue{1, 3, false} < TimeStampValue{2, 3, true});
  static_assert(TimeStampValue{1, 3, true} < TimeStampValue{2, 3, false});
  static_assert(TimeStampValue{1, 3, false} < TimeStampValue{2, 3, false});

  static_assert(TimeStampValue{1, 2, false} == TimeStampValue{1, 3, true});
  static_assert(TimeStampValue{1, 2, true} == TimeStampValue{1, 3, false});
  static_assert(TimeStampValue{1, 2, false} == TimeStampValue{1, 3, false});
};

// ----------------------------------------------------------------------------
// TimeDuration and TimeStamp implementation
// ----------------------------------------------------------------------------

MFBT_API double BaseTimeDurationPlatformUtils::ToSeconds(int64_t aTicks) {
  // Converting before arithmetic avoids blocked store forward
  return double(aTicks) / (double(sFrequencyPerSec) * 1000.0);
}

MFBT_API double BaseTimeDurationPlatformUtils::ToSecondsSigDigits(
    int64_t aTicks) {
  // don't report a value < mResolution ...
  LONGLONG resolution = sResolution;
  LONGLONG resolutionSigDigs = sResolutionSigDigs;
  LONGLONG valueSigDigs = resolution * (aTicks / resolution);
  // and chop off insignificant digits
  valueSigDigs = resolutionSigDigs * (valueSigDigs / resolutionSigDigs);
  return double(valueSigDigs) / kNsPerSecd;
}

MFBT_API int64_t
BaseTimeDurationPlatformUtils::TicksFromMilliseconds(double aMilliseconds) {
  double result = ms2mt(aMilliseconds);
  if (result > double(INT64_MAX)) {
    return INT64_MAX;
  } else if (result < double(INT64_MIN)) {
    return INT64_MIN;
  }

  return result;
}

MFBT_API int64_t BaseTimeDurationPlatformUtils::ResolutionInTicks() {
  return static_cast<int64_t>(sResolution);
}

static bool HasStableTSC() {
#if defined(_M_ARM64)
  // AArch64 defines that its system counter run at a constant rate
  // regardless of the current clock frequency of the system.  See "The
  // Generic Timer", section D7, in the ARMARM for ARMv8.
  return true;
#else
  union {
    int regs[4];
    struct {
      int nIds;
      char cpuString[12];
    };
  } cpuInfo;

  __cpuid(cpuInfo.regs, 0);
  // Only allow Intel or AMD CPUs for now.
  // The order of the registers is reg[1], reg[3], reg[2].  We just adjust the
  // string so that we can compare in one go.
  if (_strnicmp(cpuInfo.cpuString, "GenuntelineI", sizeof(cpuInfo.cpuString)) &&
      _strnicmp(cpuInfo.cpuString, "AuthcAMDenti", sizeof(cpuInfo.cpuString))) {
    return false;
  }

  int regs[4];

  // detect if the Advanced Power Management feature is supported
  __cpuid(regs, 0x80000000);
  if ((unsigned int)regs[0] < 0x80000007) {
    // XXX should we return true here?  If there is no APM there may be
    // no way how TSC can run out of sync among cores.
    return false;
  }

  __cpuid(regs, 0x80000007);
  // if bit 8 is set than TSC will run at a constant rate
  // in all ACPI P-states, C-states and T-states
  return regs[3] & (1 << 8);
#endif
}

static bool gInitialized = false;

MFBT_API void TimeStamp::Startup() {
  if (gInitialized) {
    return;
  }

  gInitialized = true;

  // Decide which implementation to use for the high-performance timer.

  InitializeCriticalSectionAndSpinCount(&sTimeStampLock, kLockSpinCount);

  bool forceGTC = false;
  bool forceQPC = false;

  char* modevar = getenv("MOZ_TIMESTAMP_MODE");
  if (modevar) {
    if (!strcmp(modevar, "QPC")) {
      forceQPC = true;
    } else if (!strcmp(modevar, "GTC")) {
      forceGTC = true;
    }
  }

  LARGE_INTEGER freq;
  sUseQPC = !forceGTC && ::QueryPerformanceFrequency(&freq);
  if (!sUseQPC) {
    // No Performance Counter.  Fall back to use GetTickCount64.
    InitResolution();

    LOG(("TimeStamp: using GetTickCount64"));
    return;
  }

  sHasStableTSC = forceQPC || HasStableTSC();
  LOG(("TimeStamp: HasStableTSC=%d", sHasStableTSC));

  sFrequencyPerSec = freq.QuadPart;
  LOG(("TimeStamp: QPC frequency=%llu", sFrequencyPerSec));

  InitThresholds();
  InitResolution();

  return;
}

MFBT_API void TimeStamp::Shutdown() { DeleteCriticalSection(&sTimeStampLock); }

TimeStampValue NowInternal(bool aHighResolution) {
  // sUseQPC is volatile
  bool useQPC = (aHighResolution && sUseQPC);

  // Both values are in [mt] units.
  ULONGLONG QPC = useQPC ? PerformanceCounter() : uint64_t(0);
  ULONGLONG GTC = ms2mt(GetTickCount64());
  return TimeStampValue(GTC, QPC, useQPC);
}

MFBT_API TimeStamp TimeStamp::Now(bool aHighResolution) {
  return TimeStamp(NowInternal(aHighResolution));
}

// Computes and returns the process uptime in microseconds.
// Returns 0 if an error was encountered.

MFBT_API uint64_t TimeStamp::ComputeProcessUptime() {
  FILETIME start, foo, bar, baz;
  bool success = GetProcessTimes(GetCurrentProcess(), &start, &foo, &bar, &baz);
  if (!success) {
    return 0;
  }

  static const StaticDynamicallyLinkedFunctionPtr<void(WINAPI*)(LPFILETIME)>
      pGetSystemTimePreciseAsFileTime(L"kernel32.dll",
                                      "GetSystemTimePreciseAsFileTime");

  FILETIME now;
  if (pGetSystemTimePreciseAsFileTime) {
    pGetSystemTimePreciseAsFileTime(&now);
  } else {
    GetSystemTimeAsFileTime(&now);
  }

  ULARGE_INTEGER startUsec = {{start.dwLowDateTime, start.dwHighDateTime}};
  ULARGE_INTEGER nowUsec = {{now.dwLowDateTime, now.dwHighDateTime}};

  return (nowUsec.QuadPart - startUsec.QuadPart) / 10ULL;
}

}  // namespace mozilla
