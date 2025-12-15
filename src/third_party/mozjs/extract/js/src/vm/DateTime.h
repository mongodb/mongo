/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_DateTime_h
#define vm_DateTime_h

#include "mozilla/Atomics.h"
#include "mozilla/UniquePtr.h"

#include <stdint.h>

#include "js/Utility.h"
#include "threading/ExclusiveData.h"

#if JS_HAS_INTL_API
#  include "mozilla/intl/ICU4CGlue.h"
#  include "mozilla/intl/TimeZone.h"
#endif

namespace JS {
class Realm;
}

namespace js {

/**
 * 21.4.1.2 Time-related Constants
 *
 * ES2025 draft rev 76814cbd5d7842c2a99d28e6e8c7833f1de5bee0
 */
constexpr int32_t HoursPerDay = 24;
constexpr int32_t MinutesPerHour = 60;
constexpr int32_t SecondsPerMinute = 60;
constexpr int32_t msPerSecond = 1000;
constexpr int32_t msPerMinute = msPerSecond * SecondsPerMinute;
constexpr int32_t msPerHour = msPerMinute * MinutesPerHour;
constexpr int32_t msPerDay = msPerHour * HoursPerDay;

/*
 * Additional quantities not mentioned in the spec.
 */
constexpr int32_t SecondsPerHour = 60 * 60;
constexpr int32_t SecondsPerDay = SecondsPerHour * 24;

constexpr double StartOfTime = -8.64e15;
constexpr double EndOfTime = 8.64e15;

extern bool InitDateTimeState();

extern void FinishDateTimeState();

enum class ResetTimeZoneMode : bool {
  DontResetIfOffsetUnchanged,
  ResetEvenIfOffsetUnchanged,
};

/**
 * Engine-internal variant of JS::ResetTimeZone with an additional flag to
 * control whether to forcibly reset all time zone data (this is the default
 * behavior when calling JS::ResetTimeZone) or to try to reuse the previous
 * time zone data.
 */
extern void ResetTimeZoneInternal(ResetTimeZoneMode mode);

/**
 * Stores date/time information, particularly concerning the current local
 * time zone, and implements a small cache for daylight saving time offset
 * computation.
 *
 * The basic idea is premised upon this fact: the DST offset never changes more
 * than once in any thirty-day period.  If we know the offset at t_0 is o_0,
 * the offset at [t_1, t_2] is also o_0, where t_1 + 3_0 days == t_2,
 * t_1 <= t_0, and t0 <= t2.  (In other words, t_0 is always somewhere within a
 * thirty-day range where the DST offset is constant: DST changes never occur
 * more than once in any thirty-day period.)  Therefore, if we intelligently
 * retain knowledge of the offset for a range of dates (which may vary over
 * time), and if requests are usually for dates within that range, we can often
 * provide a response without repeated offset calculation.
 *
 * Our caching strategy is as follows: on the first request at date t_0 compute
 * the requested offset o_0.  Save { start: t_0, end: t_0, offset: o_0 } as the
 * cache's state.  Subsequent requests within that range are straightforwardly
 * handled.  If a request for t_i is far outside the range (more than thirty
 * days), compute o_i = dstOffset(t_i) and save { start: t_i, end: t_i,
 * offset: t_i }.  Otherwise attempt to *overextend* the range to either
 * [start - 30d, end] or [start, end + 30d] as appropriate to encompass
 * t_i.  If the offset o_i30 is the same as the cached offset, extend the
 * range.  Otherwise the over-guess crossed a DST change -- compute
 * o_i = dstOffset(t_i) and either extend the original range (if o_i == offset)
 * or start a new one beneath/above the current one with o_i30 as the offset.
 *
 * This cache strategy results in 0 to 2 DST offset computations.  The naive
 * always-compute strategy is 1 computation, and since cache maintenance is a
 * handful of integer arithmetic instructions the speed difference between
 * always-1 and 1-with-cache is negligible.  Caching loses if two computations
 * happen: when the date is within 30 days of the cached range and when that
 * 30-day range crosses a DST change.  This is relatively uncommon.  Further,
 * instances of such are often dominated by in-range hits, so caching is an
 * overall slight win.
 *
 * Why 30 days?  For correctness the duration must be smaller than any possible
 * duration between DST changes.  Past that, note that 1) a large duration
 * increases the likelihood of crossing a DST change while reducing the number
 * of cache misses, and 2) a small duration decreases the size of the cached
 * range while producing more misses.  Using a month as the interval change is
 * a balance between these two that tries to optimize for the calendar month at
 * a time that a site might display.  (One could imagine an adaptive duration
 * that accommodates near-DST-change dates better; we don't believe the
 * potential win from better caching offsets the loss from extra complexity.)
 */
class DateTimeInfo {
 public:
  // For realms that force the UTC time zone (for fingerprinting protection) a
  // separate DateTimeInfo instance is used that is always in the UTC time zone.
  enum class ForceUTC { No, Yes };

 private:
  static ExclusiveData<DateTimeInfo>* instance;
  static ExclusiveData<DateTimeInfo>* instanceUTC;

  static constexpr int32_t InvalidOffset = INT32_MIN;

  // Additional cache to avoid the mutex overhead. Uses "relaxed" semantics
  // because it's acceptable if time zone offset changes aren't propagated right
  // away to all other threads.
  static inline mozilla::Atomic<int32_t, mozilla::Relaxed>
      utcToLocalOffsetSeconds{InvalidOffset};

  friend class ExclusiveData<DateTimeInfo>;

  friend bool InitDateTimeState();
  friend void FinishDateTimeState();

  explicit DateTimeInfo(bool forceUTC);
  ~DateTimeInfo();

  static auto acquireLockWithValidTimeZone(ForceUTC forceUTC) {
    auto guard =
        forceUTC == ForceUTC::Yes ? instanceUTC->lock() : instance->lock();
    if (guard->timeZoneStatus_ != TimeZoneStatus::Valid) {
      guard->updateTimeZone();
    }
    return guard;
  }

 public:
  static ForceUTC forceUTC(JS::Realm* realm);

  // The spec implicitly assumes DST and time zone adjustment information
  // never change in the course of a function -- sometimes even across
  // reentrancy.  So make critical sections as narrow as possible.

  /**
   * Get the DST offset in milliseconds at a UTC time.  This is usually
   * either 0 or |msPerSecond * SecondsPerHour|, but at least one exotic time
   * zone (Lord Howe Island, Australia) has a fractional-hour offset, just to
   * keep things interesting.
   */
  static int32_t getDSTOffsetMilliseconds(ForceUTC forceUTC,
                                          int64_t utcMilliseconds) {
    auto guard = acquireLockWithValidTimeZone(forceUTC);
    return guard->internalGetDSTOffsetMilliseconds(utcMilliseconds);
  }

  /**
   * The offset in seconds from the current UTC time to the current local
   * standard time (i.e. not including any offset due to DST) as computed by the
   * operating system.
   */
  static int32_t utcToLocalStandardOffsetSeconds(ForceUTC forceUTC) {
    // UTC offset is always zero.
    if (forceUTC == ForceUTC::Yes) {
      return 0;
    }

    // First try the cached offset to avoid any mutex overhead.
    int32_t offset = utcToLocalOffsetSeconds;
    if (offset != InvalidOffset) {
      return offset;
    }

    // If that fails, use the mutex-synchronized code path.
    auto guard = acquireLockWithValidTimeZone(forceUTC);
    offset = guard->utcToLocalStandardOffsetSeconds_;
    utcToLocalOffsetSeconds = offset;
    return offset;
  }

  enum class TimeZoneOffset { UTC, Local };

#if JS_HAS_INTL_API
  /**
   * Return the time zone offset, including DST, in milliseconds at the
   * given time. The input time can be either at UTC or at local time.
   */
  static int32_t getOffsetMilliseconds(ForceUTC forceUTC, int64_t milliseconds,
                                       TimeZoneOffset offset) {
    auto guard = acquireLockWithValidTimeZone(forceUTC);
    return guard->internalGetOffsetMilliseconds(milliseconds, offset);
  }

  /**
   * Copy the display name for the current time zone at the given time,
   * localized for the specified locale, into the supplied buffer. If the
   * buffer is too small, an empty string is stored. The stored display name
   * is null-terminated in any case.
   */
  static bool timeZoneDisplayName(ForceUTC forceUTC, char16_t* buf,
                                  size_t buflen, int64_t utcMilliseconds,
                                  const char* locale) {
    auto guard = acquireLockWithValidTimeZone(forceUTC);
    return guard->internalTimeZoneDisplayName(buf, buflen, utcMilliseconds,
                                              locale);
  }

  /**
   * Copy the identifier for the current time zone to the provided resizable
   * buffer.
   */
  template <typename B>
  static mozilla::intl::ICUResult timeZoneId(ForceUTC forceUTC, B& buffer) {
    auto guard = acquireLockWithValidTimeZone(forceUTC);
    return guard->timeZone()->GetId(buffer);
  }

  /**
   * A number indicating the raw offset from GMT in milliseconds.
   */
  static mozilla::Result<int32_t, mozilla::intl::ICUError> getRawOffsetMs(
      ForceUTC forceUTC) {
    auto guard = acquireLockWithValidTimeZone(forceUTC);
    return guard->timeZone()->GetRawOffsetMs();
  }
#else
  /**
   * Return the local time zone adjustment (ES2019 20.3.1.7) as computed by
   * the operating system.
   */
  static int32_t localTZA(ForceUTC forceUTC) {
    return utcToLocalStandardOffsetSeconds(forceUTC) * msPerSecond;
  }
#endif /* JS_HAS_INTL_API */

  // JIT access.
  static const void* addressOfUTCToLocalOffsetSeconds() {
    static_assert(sizeof(decltype(utcToLocalOffsetSeconds)) == sizeof(int32_t));
    return &DateTimeInfo::utcToLocalOffsetSeconds;
  }

 private:
  // The method below should only be called via js::ResetTimeZoneInternal().
  friend void js::ResetTimeZoneInternal(ResetTimeZoneMode);

  static void resetTimeZone(ResetTimeZoneMode mode) {
    {
      auto guard = instance->lock();
      guard->internalResetTimeZone(mode);

      // Mark the cached value as invalid.
      utcToLocalOffsetSeconds = InvalidOffset;
    }
    {
      // Only needed to initialize the default state and any later call will
      // perform an unnecessary reset.
      auto guard = instanceUTC->lock();
      guard->internalResetTimeZone(mode);
    }
  }

  struct RangeCache {
    // Start and end offsets in seconds describing the current and the
    // last cached range.
    int64_t startSeconds, endSeconds;
    int64_t oldStartSeconds, oldEndSeconds;

    // The current and the last cached offset in milliseconds.
    int32_t offsetMilliseconds;
    int32_t oldOffsetMilliseconds;

    void reset();

    void sanityCheck();
  };

  bool forceUTC_;

  enum class TimeZoneStatus : uint8_t { Valid, NeedsUpdate, UpdateIfChanged };

  TimeZoneStatus timeZoneStatus_;

  /**
   * The offset in seconds from the current UTC time to the current local
   * standard time (i.e. not including any offset due to DST) as computed by the
   * operating system.
   *
   * Cached because retrieving this dynamically is Slow, and a certain venerable
   * benchmark which shall not be named depends on it being fast.
   *
   * SpiderMonkey occasionally and arbitrarily updates this value from the
   * system time zone to attempt to keep this reasonably up-to-date.  If
   * temporary inaccuracy can't be tolerated, JSAPI clients may call
   * JS::ResetTimeZone to forcibly sync this with the system time zone.
   *
   * In most cases this value is consistent with the raw time zone offset as
   * returned by the ICU default time zone (`icu::TimeZone::getRawOffset()`),
   * but it is possible to create cases where the operating system default time
   * zone differs from the ICU default time zone. For example ICU doesn't
   * support the full range of TZ environment variable settings, which can
   * result in <ctime> returning a different time zone than what's returned by
   * ICU. One example is "TZ=WGT3WGST,M3.5.0/-2,M10.5.0/-1", where <ctime>
   * returns -3 hours as the local offset, but ICU flat out rejects the TZ value
   * and instead infers the default time zone via "/etc/localtime" (on Unix).
   * This offset can also differ from ICU when the operating system and ICU use
   * different tzdata versions and the time zone rules of the current system
   * time zone have changed. Or, on Windows, when the Windows default time zone
   * can't be mapped to a IANA time zone, see for example
   * <https://unicode-org.atlassian.net/browse/ICU-13845>.
   *
   * When ICU is exclusively used for time zone computations, that means when
   * |JS_HAS_INTL_API| is true, this field is only used to detect system default
   * time zone changes. It must not be used to convert between local and UTC
   * time, because, as outlined above, this could lead to different results when
   * compared to ICU.
   */
  int32_t utcToLocalStandardOffsetSeconds_;

  RangeCache dstRange_;  // UTC-based ranges

#if JS_HAS_INTL_API
  // Use the full date-time range when we can use mozilla::intl::TimeZone.
  static constexpr int64_t MinTimeT =
      static_cast<int64_t>(StartOfTime / msPerSecond);
  static constexpr int64_t MaxTimeT =
      static_cast<int64_t>(EndOfTime / msPerSecond);

  RangeCache utcRange_;    // localtime-based ranges
  RangeCache localRange_;  // UTC-based ranges

  /**
   * The current time zone. Lazily constructed to avoid potential I/O access
   * when initializing this class.
   */
  mozilla::UniquePtr<mozilla::intl::TimeZone> timeZone_;

  /**
   * Cached names of the standard and daylight savings display names of the
   * current time zone for the default locale.
   */
  JS::UniqueChars locale_;
  JS::UniqueTwoByteChars standardName_;
  JS::UniqueTwoByteChars daylightSavingsName_;
#else
  // Restrict the data-time range to the minimum required time_t range as
  // specified in POSIX. Most operating systems support 64-bit time_t
  // values, but we currently still have some configurations which use
  // 32-bit time_t, e.g. the ARM simulator on 32-bit Linux (bug 1406993).
  // Bug 1406992 explores to use 64-bit time_t when supported by the
  // underlying operating system.
  static constexpr int64_t MinTimeT = 0;          /* time_t 01/01/1970 */
  static constexpr int64_t MaxTimeT = 2145830400; /* time_t 12/31/2037 */
#endif /* JS_HAS_INTL_API */

  static constexpr int64_t RangeExpansionAmount = 30 * SecondsPerDay;

  void internalResetTimeZone(ResetTimeZoneMode mode);

  void updateTimeZone();

  void internalResyncICUDefaultTimeZone();

  int64_t toClampedSeconds(int64_t milliseconds);

  using ComputeFn = int32_t (DateTimeInfo::*)(int64_t);

  /**
   * Get or compute an offset value for the requested seconds value.
   */
  int32_t getOrComputeValue(RangeCache& range, int64_t seconds,
                            ComputeFn compute);

  /**
   * Compute the DST offset at the given UTC time in seconds from the epoch.
   * (getDSTOffsetMilliseconds attempts to return a cached value from the
   * dstRange_ member, but in case of a cache miss it calls this method.)
   */
  int32_t computeDSTOffsetMilliseconds(int64_t utcSeconds);

  int32_t internalGetDSTOffsetMilliseconds(int64_t utcMilliseconds);

#if JS_HAS_INTL_API
  /**
   * Compute the UTC offset in milliseconds for the given local time. Called
   * by internalGetOffsetMilliseconds on a cache miss.
   */
  int32_t computeUTCOffsetMilliseconds(int64_t localSeconds);

  /**
   * Compute the local time offset in milliseconds for the given UTC time.
   * Called by internalGetOffsetMilliseconds on a cache miss.
   */
  int32_t computeLocalOffsetMilliseconds(int64_t utcSeconds);

  int32_t internalGetOffsetMilliseconds(int64_t milliseconds,
                                        TimeZoneOffset offset);

  bool internalTimeZoneDisplayName(char16_t* buf, size_t buflen,
                                   int64_t utcMilliseconds, const char* locale);

  mozilla::intl::TimeZone* timeZone();
#endif /* JS_HAS_INTL_API */
};

} /* namespace js */

#endif /* vm_DateTime_h */
