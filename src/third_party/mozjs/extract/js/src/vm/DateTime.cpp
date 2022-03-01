/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/DateTime.h"

#include "mozilla/ScopeExit.h"
#include "mozilla/TextUtils.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <time.h>

#if !defined(XP_WIN)
#  include <limits.h>
#  include <unistd.h>
#endif /* !defined(XP_WIN) */

#include "js/Date.h"
#include "js/GCAPI.h"
#include "threading/ExclusiveData.h"

#if JS_HAS_INTL_API && !MOZ_SYSTEM_ICU
#  include "unicode/basictz.h"
#  include "unicode/locid.h"
#  include "unicode/timezone.h"
#  include "unicode/unistr.h"
#endif /* JS_HAS_INTL_API && !MOZ_SYSTEM_ICU */

#include "util/Text.h"
#include "vm/MutexIDs.h"

static bool ComputeLocalTime(time_t local, struct tm* ptm) {
#if defined(_WIN32)
  return localtime_s(ptm, &local) == 0;
#elif defined(HAVE_LOCALTIME_R)
  return localtime_r(&local, ptm);
#else
  struct tm* otm = localtime(&local);
  if (!otm) {
    return false;
  }
  *ptm = *otm;
  return true;
#endif
}

static bool ComputeUTCTime(time_t t, struct tm* ptm) {
#if defined(_WIN32)
  return gmtime_s(ptm, &t) == 0;
#elif defined(HAVE_GMTIME_R)
  return gmtime_r(&t, ptm);
#else
  struct tm* otm = gmtime(&t);
  if (!otm) {
    return false;
  }
  *ptm = *otm;
  return true;
#endif
}

/*
 * Compute the offset in seconds from the current UTC time to the current local
 * standard time (i.e. not including any offset due to DST).
 *
 * Examples:
 *
 * Suppose we are in California, USA on January 1, 2013 at 04:00 PST (UTC-8, no
 * DST in effect), corresponding to 12:00 UTC.  This function would then return
 * -8 * SecondsPerHour, or -28800.
 *
 * Or suppose we are in Berlin, Germany on July 1, 2013 at 17:00 CEST (UTC+2,
 * DST in effect), corresponding to 15:00 UTC.  This function would then return
 * +1 * SecondsPerHour, or +3600.
 */
static int32_t UTCToLocalStandardOffsetSeconds() {
  using js::SecondsPerDay;
  using js::SecondsPerHour;
  using js::SecondsPerMinute;

  // Get the current time.
  time_t currentMaybeWithDST = time(nullptr);
  if (currentMaybeWithDST == time_t(-1)) {
    return 0;
  }

  // Break down the current time into its (locally-valued, maybe with DST)
  // components.
  struct tm local;
  if (!ComputeLocalTime(currentMaybeWithDST, &local)) {
    return 0;
  }

  // Compute a |time_t| corresponding to |local| interpreted without DST.
  time_t currentNoDST;
  if (local.tm_isdst == 0) {
    // If |local| wasn't DST, we can use the same time.
    currentNoDST = currentMaybeWithDST;
  } else {
    // If |local| respected DST, we need a time broken down into components
    // ignoring DST.  Turn off DST in the broken-down time.  Create a fresh
    // copy of |local|, because mktime() will reset tm_isdst = 1 and will
    // adjust tm_hour and tm_hour accordingly.
    struct tm localNoDST = local;
    localNoDST.tm_isdst = 0;

    // Compute a |time_t t| corresponding to the broken-down time with DST
    // off.  This has boundary-condition issues (for about the duration of
    // a DST offset) near the time a location moves to a different time
    // zone.  But 1) errors will be transient; 2) locations rarely change
    // time zone; and 3) in the absence of an API that provides the time
    // zone offset directly, this may be the best we can do.
    currentNoDST = mktime(&localNoDST);
    if (currentNoDST == time_t(-1)) {
      return 0;
    }
  }

  // Break down the time corresponding to the no-DST |local| into UTC-based
  // components.
  struct tm utc;
  if (!ComputeUTCTime(currentNoDST, &utc)) {
    return 0;
  }

  // Finally, compare the seconds-based components of the local non-DST
  // representation and the UTC representation to determine the actual
  // difference.
  int utc_secs = utc.tm_hour * SecondsPerHour + utc.tm_min * SecondsPerMinute;
  int local_secs =
      local.tm_hour * SecondsPerHour + local.tm_min * SecondsPerMinute;

  // Same-day?  Just subtract the seconds counts.
  if (utc.tm_mday == local.tm_mday) {
    return local_secs - utc_secs;
  }

  // If we have more UTC seconds, move local seconds into the UTC seconds'
  // frame of reference and then subtract.
  if (utc_secs > local_secs) {
    return (SecondsPerDay + local_secs) - utc_secs;
  }

  // Otherwise we have more local seconds, so move the UTC seconds into the
  // local seconds' frame of reference and then subtract.
  return local_secs - (utc_secs + SecondsPerDay);
}

void js::DateTimeInfo::internalResetTimeZone(ResetTimeZoneMode mode) {
  // Nothing to do when an update request is already enqueued.
  if (timeZoneStatus_ == TimeZoneStatus::NeedsUpdate) {
    return;
  }

  // Mark the state as needing an update, but defer the actual update until it's
  // actually needed to delay any system calls to the last possible moment. This
  // is beneficial when this method is called during start-up, because it avoids
  // main-thread I/O blocking the process.
  if (mode == ResetTimeZoneMode::ResetEvenIfOffsetUnchanged) {
    timeZoneStatus_ = TimeZoneStatus::NeedsUpdate;
  } else {
    timeZoneStatus_ = TimeZoneStatus::UpdateIfChanged;
  }
}

void js::DateTimeInfo::updateTimeZone() {
  MOZ_ASSERT(timeZoneStatus_ != TimeZoneStatus::Valid);

  bool updateIfChanged = timeZoneStatus_ == TimeZoneStatus::UpdateIfChanged;

  timeZoneStatus_ = TimeZoneStatus::Valid;

  /*
   * The difference between local standard time and UTC will never change for
   * a given time zone.
   */
  int32_t newOffset = UTCToLocalStandardOffsetSeconds();

  if (updateIfChanged && newOffset == utcToLocalStandardOffsetSeconds_) {
    return;
  }

  utcToLocalStandardOffsetSeconds_ = newOffset;

  dstRange_.reset();

#if JS_HAS_INTL_API && !MOZ_SYSTEM_ICU
  utcRange_.reset();
  localRange_.reset();

  {
    // Tell the analysis the |pFree| function pointer called by uprv_free
    // cannot GC.
    JS::AutoSuppressGCAnalysis nogc;

    timeZone_ = nullptr;
  }

  standardName_ = nullptr;
  daylightSavingsName_ = nullptr;
#endif /* JS_HAS_INTL_API && !MOZ_SYSTEM_ICU */

  // Propagate the time zone change to ICU, too.
  {
    // Tell the analysis calling into ICU cannot GC.
    JS::AutoSuppressGCAnalysis nogc;

    internalResyncICUDefaultTimeZone();
  }
}

js::DateTimeInfo::DateTimeInfo() {
  // Set the time zone status into the invalid state, so we compute the actual
  // defaults on first access. We don't yet want to initialize neither <ctime>
  // nor ICU's time zone classes, because that may cause I/O operations slowing
  // down the JS engine initialization, which we're currently in the middle of.
  timeZoneStatus_ = TimeZoneStatus::NeedsUpdate;
}

js::DateTimeInfo::~DateTimeInfo() = default;

int64_t js::DateTimeInfo::toClampedSeconds(int64_t milliseconds) {
  int64_t seconds = milliseconds / msPerSecond;
  if (seconds > MaxTimeT) {
    seconds = MaxTimeT;
  } else if (seconds < MinTimeT) {
    /* Go ahead a day to make localtime work (does not work with 0). */
    seconds = SecondsPerDay;
  }
  return seconds;
}

int32_t js::DateTimeInfo::computeDSTOffsetMilliseconds(int64_t utcSeconds) {
  MOZ_ASSERT(utcSeconds >= MinTimeT);
  MOZ_ASSERT(utcSeconds <= MaxTimeT);

#if JS_HAS_INTL_API && !MOZ_SYSTEM_ICU
  UDate date = UDate(utcSeconds * msPerSecond);
  constexpr bool dateIsLocalTime = false;
  int32_t rawOffset, dstOffset;
  UErrorCode status = U_ZERO_ERROR;

  timeZone()->getOffset(date, dateIsLocalTime, rawOffset, dstOffset, status);
  if (U_FAILURE(status)) {
    return 0;
  }

  return dstOffset;
#else
  struct tm tm;
  if (!ComputeLocalTime(static_cast<time_t>(utcSeconds), &tm)) {
    return 0;
  }

  // NB: The offset isn't computed correctly when the standard local offset
  //     at |utcSeconds| is different from |utcToLocalStandardOffsetSeconds|.
  int32_t dayoff =
      int32_t((utcSeconds + utcToLocalStandardOffsetSeconds_) % SecondsPerDay);
  int32_t tmoff = tm.tm_sec + (tm.tm_min * SecondsPerMinute) +
                  (tm.tm_hour * SecondsPerHour);

  int32_t diff = tmoff - dayoff;

  if (diff < 0) {
    diff += SecondsPerDay;
  } else if (uint32_t(diff) >= SecondsPerDay) {
    diff -= SecondsPerDay;
  }

  return diff * msPerSecond;
#endif /* JS_HAS_INTL_API && !MOZ_SYSTEM_ICU */
}

int32_t js::DateTimeInfo::internalGetDSTOffsetMilliseconds(
    int64_t utcMilliseconds) {
  int64_t utcSeconds = toClampedSeconds(utcMilliseconds);
  return getOrComputeValue(dstRange_, utcSeconds,
                           &DateTimeInfo::computeDSTOffsetMilliseconds);
}

int32_t js::DateTimeInfo::getOrComputeValue(RangeCache& range, int64_t seconds,
                                            ComputeFn compute) {
  range.sanityCheck();

  auto checkSanity =
      mozilla::MakeScopeExit([&range]() { range.sanityCheck(); });

  // NB: Be aware of the initial range values when making changes to this
  //     code: the first call to this method, with those initial range
  //     values, must result in a cache miss.
  MOZ_ASSERT(seconds != INT64_MIN);

  if (range.startSeconds <= seconds && seconds <= range.endSeconds) {
    return range.offsetMilliseconds;
  }

  if (range.oldStartSeconds <= seconds && seconds <= range.oldEndSeconds) {
    return range.oldOffsetMilliseconds;
  }

  range.oldOffsetMilliseconds = range.offsetMilliseconds;
  range.oldStartSeconds = range.startSeconds;
  range.oldEndSeconds = range.endSeconds;

  if (range.startSeconds <= seconds) {
    int64_t newEndSeconds =
        std::min({range.endSeconds + RangeExpansionAmount, MaxTimeT});
    if (newEndSeconds >= seconds) {
      int32_t endOffsetMilliseconds = (this->*compute)(newEndSeconds);
      if (endOffsetMilliseconds == range.offsetMilliseconds) {
        range.endSeconds = newEndSeconds;
        return range.offsetMilliseconds;
      }

      range.offsetMilliseconds = (this->*compute)(seconds);
      if (range.offsetMilliseconds == endOffsetMilliseconds) {
        range.startSeconds = seconds;
        range.endSeconds = newEndSeconds;
      } else {
        range.endSeconds = seconds;
      }
      return range.offsetMilliseconds;
    }

    range.offsetMilliseconds = (this->*compute)(seconds);
    range.startSeconds = range.endSeconds = seconds;
    return range.offsetMilliseconds;
  }

  int64_t newStartSeconds =
      std::max<int64_t>({range.startSeconds - RangeExpansionAmount, MinTimeT});
  if (newStartSeconds <= seconds) {
    int32_t startOffsetMilliseconds = (this->*compute)(newStartSeconds);
    if (startOffsetMilliseconds == range.offsetMilliseconds) {
      range.startSeconds = newStartSeconds;
      return range.offsetMilliseconds;
    }

    range.offsetMilliseconds = (this->*compute)(seconds);
    if (range.offsetMilliseconds == startOffsetMilliseconds) {
      range.startSeconds = newStartSeconds;
      range.endSeconds = seconds;
    } else {
      range.startSeconds = seconds;
    }
    return range.offsetMilliseconds;
  }

  range.startSeconds = range.endSeconds = seconds;
  range.offsetMilliseconds = (this->*compute)(seconds);
  return range.offsetMilliseconds;
}

void js::DateTimeInfo::RangeCache::reset() {
  // The initial range values are carefully chosen to result in a cache miss
  // on first use given the range of possible values. Be careful to keep
  // these values and the caching algorithm in sync!
  offsetMilliseconds = 0;
  startSeconds = endSeconds = INT64_MIN;
  oldOffsetMilliseconds = 0;
  oldStartSeconds = oldEndSeconds = INT64_MIN;

  sanityCheck();
}

void js::DateTimeInfo::RangeCache::sanityCheck() {
  auto assertRange = [](int64_t start, int64_t end) {
    MOZ_ASSERT(start <= end);
    MOZ_ASSERT_IF(start == INT64_MIN, end == INT64_MIN);
    MOZ_ASSERT_IF(end == INT64_MIN, start == INT64_MIN);
    MOZ_ASSERT_IF(start != INT64_MIN, start >= MinTimeT && end >= MinTimeT);
    MOZ_ASSERT_IF(start != INT64_MIN, start <= MaxTimeT && end <= MaxTimeT);
  };

  assertRange(startSeconds, endSeconds);
  assertRange(oldStartSeconds, oldEndSeconds);
}

#if JS_HAS_INTL_API && !MOZ_SYSTEM_ICU
int32_t js::DateTimeInfo::computeUTCOffsetMilliseconds(int64_t localSeconds) {
  MOZ_ASSERT(localSeconds >= MinTimeT);
  MOZ_ASSERT(localSeconds <= MaxTimeT);

  UDate date = UDate(localSeconds * msPerSecond);

  // ES2019 draft rev 0ceb728a1adbffe42b26972a6541fd7f398b1557
  //
  // 20.3.1.7 LocalTZA
  //
  // If |localSeconds| represents either a skipped (at a positive time zone
  // transition) or repeated (at a negative time zone transition) locale
  // time, it must be interpreted as a time value before the transition.
  constexpr int32_t skippedTime = icu::BasicTimeZone::kFormer;
  constexpr int32_t repeatedTime = icu::BasicTimeZone::kFormer;

  int32_t rawOffset, dstOffset;
  UErrorCode status = U_ZERO_ERROR;

  // All ICU TimeZone classes derive from BasicTimeZone, so we can safely
  // perform the static_cast.
  // Once <https://unicode-org.atlassian.net/browse/ICU-13705> is fixed we
  // can remove this extra cast.
  auto* basicTz = static_cast<icu::BasicTimeZone*>(timeZone());
  basicTz->getOffsetFromLocal(date, skippedTime, repeatedTime, rawOffset,
                              dstOffset, status);
  if (U_FAILURE(status)) {
    return 0;
  }

  return rawOffset + dstOffset;
}

int32_t js::DateTimeInfo::computeLocalOffsetMilliseconds(int64_t utcSeconds) {
  MOZ_ASSERT(utcSeconds >= MinTimeT);
  MOZ_ASSERT(utcSeconds <= MaxTimeT);

  UDate date = UDate(utcSeconds * msPerSecond);
  constexpr bool dateIsLocalTime = false;
  int32_t rawOffset, dstOffset;
  UErrorCode status = U_ZERO_ERROR;

  timeZone()->getOffset(date, dateIsLocalTime, rawOffset, dstOffset, status);
  if (U_FAILURE(status)) {
    return 0;
  }

  return rawOffset + dstOffset;
}

int32_t js::DateTimeInfo::internalGetOffsetMilliseconds(int64_t milliseconds,
                                                        TimeZoneOffset offset) {
  int64_t seconds = toClampedSeconds(milliseconds);
  return offset == TimeZoneOffset::UTC
             ? getOrComputeValue(localRange_, seconds,
                                 &DateTimeInfo::computeLocalOffsetMilliseconds)
             : getOrComputeValue(utcRange_, seconds,
                                 &DateTimeInfo::computeUTCOffsetMilliseconds);
}

bool js::DateTimeInfo::internalTimeZoneDisplayName(char16_t* buf, size_t buflen,
                                                   int64_t utcMilliseconds,
                                                   const char* locale) {
  MOZ_ASSERT(buf != nullptr);
  MOZ_ASSERT(buflen > 0);
  MOZ_ASSERT(locale != nullptr);

  // Clear any previously cached names when the default locale changed.
  if (!locale_ || std::strcmp(locale_.get(), locale) != 0) {
    locale_ = DuplicateString(locale);
    if (!locale_) {
      return false;
    }

    standardName_.reset();
    daylightSavingsName_.reset();
  }

  bool daylightSavings = internalGetDSTOffsetMilliseconds(utcMilliseconds) != 0;

  JS::UniqueTwoByteChars& cachedName =
      daylightSavings ? daylightSavingsName_ : standardName_;
  if (!cachedName) {
    // Retrieve the display name for the given locale.
    icu::UnicodeString displayName;
    timeZone()->getDisplayName(daylightSavings, icu::TimeZone::LONG,
                               icu::Locale(locale), displayName);

    size_t capacity = displayName.length() + 1;  // Null-terminate.
    JS::UniqueTwoByteChars displayNameChars(js_pod_malloc<char16_t>(capacity));
    if (!displayNameChars) {
      return false;
    }

    // Copy the display name. This operation always succeeds because the
    // destination buffer is large enough to hold the complete string.
    UErrorCode status = U_ZERO_ERROR;
    displayName.extract(displayNameChars.get(), capacity, status);
    MOZ_ASSERT(U_SUCCESS(status));
    MOZ_ASSERT(displayNameChars[capacity - 1] == '\0');

    cachedName = std::move(displayNameChars);
  }

  // Return an empty string if the display name doesn't fit into the buffer.
  size_t length = js_strlen(cachedName.get());
  if (length < buflen) {
    std::copy(cachedName.get(), cachedName.get() + length, buf);
  } else {
    length = 0;
  }

  buf[length] = '\0';
  return true;
}

icu::TimeZone* js::DateTimeInfo::timeZone() {
  if (!timeZone_) {
    timeZone_.reset(icu::TimeZone::createDefault());
    MOZ_ASSERT(timeZone_);
  }

  return timeZone_.get();
}
#endif /* JS_HAS_INTL_API && !MOZ_SYSTEM_ICU */

/* static */ js::ExclusiveData<js::DateTimeInfo>* js::DateTimeInfo::instance;

bool js::InitDateTimeState() {
  MOZ_ASSERT(!DateTimeInfo::instance, "we should be initializing only once");

  DateTimeInfo::instance =
      js_new<ExclusiveData<DateTimeInfo>>(mutexid::DateTimeInfoMutex);
  return !!DateTimeInfo::instance;
}

/* static */
void js::FinishDateTimeState() {
  js_delete(DateTimeInfo::instance);
  DateTimeInfo::instance = nullptr;
}

void js::ResetTimeZoneInternal(ResetTimeZoneMode mode) {
  js::DateTimeInfo::resetTimeZone(mode);
}

JS_PUBLIC_API void JS::ResetTimeZone() {
  js::ResetTimeZoneInternal(js::ResetTimeZoneMode::ResetEvenIfOffsetUnchanged);
}

#if defined(XP_WIN)
static bool IsOlsonCompatibleWindowsTimeZoneId(const char* tz) {
  // ICU ignores the TZ environment variable on Windows and instead directly
  // invokes Win API functions to retrieve the current time zone. But since
  // we're still using the POSIX-derived localtime_s() function on Windows
  // and localtime_s() does return a time zone adjusted value based on the
  // TZ environment variable, we need to manually adjust the default ICU
  // time zone if TZ is set.
  //
  // Windows supports the following format for TZ: tzn[+|-]hh[:mm[:ss]][dzn]
  // where "tzn" is the time zone name for standard time, the time zone
  // offset is positive for time zones west of GMT, and "dzn" is the
  // optional time zone name when daylight savings are observed. Daylight
  // savings are always based on the U.S. daylight saving rules, that means
  // for example it's not possible to use "TZ=CET-1CEST" to select the IANA
  // time zone "CET".
  //
  // When comparing this restricted format for TZ to all IANA time zone
  // names, the following time zones are in the intersection of what's
  // supported by Windows and is also a valid IANA time zone identifier.
  //
  // Even though the time zone offset is marked as mandatory on MSDN, it
  // appears it defaults to zero when omitted. This in turn means we can
  // also allow the time zone identifiers "UCT", "UTC", and "GMT".

  static const char* const allowedIds[] = {
      // From tzdata's "northamerica" file:
      "EST5EDT",
      "CST6CDT",
      "MST7MDT",
      "PST8PDT",

      // From tzdata's "backward" file:
      "GMT+0",
      "GMT-0",
      "GMT0",
      "UCT",
      "UTC",

      // From tzdata's "etcetera" file:
      "GMT",
  };
  for (const auto& allowedId : allowedIds) {
    if (std::strcmp(allowedId, tz) == 0) {
      return true;
    }
  }
  return false;
}
#elif JS_HAS_INTL_API && !MOZ_SYSTEM_ICU
static inline const char* TZContainsAbsolutePath(const char* tzVar) {
  // A TZ environment variable may be an absolute path. The path
  // format of TZ may begin with a colon. (ICU handles relative paths.)
  if (tzVar[0] == ':' && tzVar[1] == '/') {
    return tzVar + 1;
  }
  if (tzVar[0] == '/') {
    return tzVar;
  }
  return nullptr;
}

/**
 * Reject the input if it doesn't match the time zone id pattern or legacy time
 * zone names.
 *
 * See <https://github.com/eggert/tz/blob/master/theory.html>.
 */
static icu::UnicodeString MaybeTimeZoneId(const char* timeZone) {
  size_t timeZoneLen = std::strlen(timeZone);

  for (size_t i = 0; i < timeZoneLen; i++) {
    char c = timeZone[i];

    // According to theory.html, '.' is allowed in time zone ids, but the
    // accompanying zic.c file doesn't allow it. Assume the source file is
    // correct and disallow '.' here, too.
    if (mozilla::IsAsciiAlphanumeric(c) || c == '_' || c == '-' || c == '+') {
      continue;
    }

    // Reject leading, trailing, or consecutive '/' characters.
    if (c == '/' && i > 0 && i + 1 < timeZoneLen && timeZone[i + 1] != '/') {
      continue;
    }

    return icu::UnicodeString();
  }

  return icu::UnicodeString(timeZone, timeZoneLen, US_INV);
}

/**
 * Given a presumptive path |tz| to a zoneinfo time zone file
 * (e.g. /etc/localtime), attempt to compute the time zone encoded by that
 * path by repeatedly resolving symlinks until a path containing "/zoneinfo/"
 * followed by time zone looking components is found. If a symlink is broken,
 * symlink-following recurs too deeply, non time zone looking components are
 * encountered, or some other error is encountered, return the empty string.
 *
 * If a non-empty string is returned, it's only guaranteed to have certain
 * syntactic validity. It might not actually *be* a time zone name.
 */
static icu::UnicodeString ReadTimeZoneLink(const char* tz) {
  // The resolved link name can have different paths depending on the OS.
  // Follow ICU and only search for "/zoneinfo/"; see $ICU/common/putil.cpp.
  static constexpr char ZoneInfoPath[] = "/zoneinfo/";
  constexpr size_t ZoneInfoPathLength = js_strlen(ZoneInfoPath);

  // Stop following symlinks after a fixed depth, because some common time
  // zones are stored in files whose name doesn't match an Olson time zone
  // name. For example on Ubuntu, "/usr/share/zoneinfo/America/New_York" is a
  // symlink to "/usr/share/zoneinfo/posixrules" and "posixrules" is not an
  // Olson time zone name.
  // Four hops should be a reasonable limit for most use cases.
  constexpr uint32_t FollowDepthLimit = 4;

#  ifdef PATH_MAX
  constexpr size_t PathMax = PATH_MAX;
#  else
  constexpr size_t PathMax = 4096;
#  endif
  static_assert(PathMax > 0, "PathMax should be larger than zero");

  char linkName[PathMax];
  constexpr size_t linkNameLen =
      std::size(linkName) - 1;  // -1 to null-terminate.

  // Return if the TZ value is too large.
  if (std::strlen(tz) > linkNameLen) {
    return icu::UnicodeString();
  }

  std::strcpy(linkName, tz);

  char linkTarget[PathMax];
  constexpr size_t linkTargetLen =
      std::size(linkTarget) - 1;  // -1 to null-terminate.

  uint32_t depth = 0;

  // Search until we find "/zoneinfo/" in the link name.
  const char* timeZoneWithZoneInfo;
  while (!(timeZoneWithZoneInfo = std::strstr(linkName, ZoneInfoPath))) {
    // Return if the symlink nesting is too deep.
    if (++depth > FollowDepthLimit) {
      return icu::UnicodeString();
    }

    // Return on error or if the result was truncated.
    ssize_t slen = readlink(linkName, linkTarget, linkTargetLen);
    if (slen < 0 || size_t(slen) >= linkTargetLen) {
      return icu::UnicodeString();
    }

    // Ensure linkTarget is null-terminated. (readlink may not necessarily
    // null-terminate the string.)
    size_t len = size_t(slen);
    linkTarget[len] = '\0';

    // If the target is absolute, continue with that.
    if (linkTarget[0] == '/') {
      std::strcpy(linkName, linkTarget);
      continue;
    }

    // If the target is relative, it must be resolved against either the
    // directory the link was in, or against the current working directory.
    char* separator = std::strrchr(linkName, '/');

    // If the link name is just something like "foo", resolve linkTarget
    // against the current working directory.
    if (!separator) {
      std::strcpy(linkName, linkTarget);
      continue;
    }

    // Remove everything after the final path separator in linkName.
    separator[1] = '\0';

    // Return if the concatenated path name is too large.
    if (std::strlen(linkName) + len > linkNameLen) {
      return icu::UnicodeString();
    }

    // Keep it simple and just concatenate the path names.
    std::strcat(linkName, linkTarget);
  }

  const char* timeZone = timeZoneWithZoneInfo + ZoneInfoPathLength;
  return MaybeTimeZoneId(timeZone);
}
#endif /* JS_HAS_INTL_API && !MOZ_SYSTEM_ICU */

void js::ResyncICUDefaultTimeZone() {
  js::DateTimeInfo::resyncICUDefaultTimeZone();
}

void js::DateTimeInfo::internalResyncICUDefaultTimeZone() {
#if JS_HAS_INTL_API && !MOZ_SYSTEM_ICU
  if (const char* tz = std::getenv("TZ")) {
    icu::UnicodeString tzid;

#  if defined(XP_WIN)
    // If TZ is set and its value is valid under Windows' and IANA's time zone
    // identifier rules, update the ICU default time zone to use this value.
    if (IsOlsonCompatibleWindowsTimeZoneId(tz)) {
      tzid.setTo(icu::UnicodeString(tz, -1, US_INV));
    } else {
      // If |tz| isn't a supported time zone identifier, use the default Windows
      // time zone for ICU.
      // TODO: Handle invalid time zone identifiers (bug 342068).
    }
#  else
    // The TZ environment variable allows both absolute and relative paths,
    // optionally beginning with a colon (':'). (Relative paths, without the
    // colon, are just Olson time zone names.)  We need to handle absolute paths
    // ourselves, including handling that they might be symlinks.
    // <https://unicode-org.atlassian.net/browse/ICU-13694>
    if (const char* tzlink = TZContainsAbsolutePath(tz)) {
      tzid.setTo(ReadTimeZoneLink(tzlink));
    }

#    ifdef ANDROID
    // ICU ignores the TZ environment variable on Android. If it doesn't contain
    // an absolute path, try to parse it as a time zone name.
    else {
      tzid.setTo(MaybeTimeZoneId(tz));
    }
#    endif
#  endif /* defined(XP_WIN) */

    if (!tzid.isEmpty()) {
      mozilla::UniquePtr<icu::TimeZone> newTimeZone(
          icu::TimeZone::createTimeZone(tzid));
      MOZ_ASSERT(newTimeZone);
      if (*newTimeZone != icu::TimeZone::getUnknown()) {
        // adoptDefault() takes ownership of the time zone.
        icu::TimeZone::adoptDefault(newTimeZone.release());
        return;
      }
    }
  }

  if (icu::TimeZone* defaultZone = icu::TimeZone::detectHostTimeZone()) {
    icu::TimeZone::adoptDefault(defaultZone);
  }
#endif
}
