// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

#if defined(_WIN32) || defined(_WIN64)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "time_zone_libc.h"

#include <chrono>
#include <ctime>
#include <tuple>
#include <utility>

#include "absl/time/internal/cctz/include/cctz/civil_time.h"
#include "absl/time/internal/cctz/include/cctz/time_zone.h"

namespace absl {
namespace time_internal {
namespace cctz {

namespace {

// .first is seconds east of UTC; .second is the time-zone abbreviation.
using OffsetAbbr = std::pair<int, const char*>;

// Defines a function that can be called as follows:
//
//   std::tm tm = ...;
//   OffsetAbbr off_abbr = get_offset_abbr(tm);
//
#if defined(_WIN32) || defined(_WIN64)
// Uses the globals: '_timezone', '_dstbias' and '_tzname'.
OffsetAbbr get_offset_abbr(const std::tm& tm) {
  const bool is_dst = tm.tm_isdst > 0;
  const int off = _timezone + (is_dst ? _dstbias : 0);
  const char* abbr = _tzname[is_dst];
  return {off, abbr};
}
#elif defined(__sun)
// Uses the globals: 'timezone', 'altzone' and 'tzname'.
OffsetAbbr get_offset_abbr(const std::tm& tm) {
  const bool is_dst = tm.tm_isdst > 0;
  const int off = is_dst ? altzone : timezone;
  const char* abbr = tzname[is_dst];
  return {off, abbr};
}
#elif defined(__native_client__) || defined(__myriad2__) || \
    defined(__EMSCRIPTEN__)
// Uses the globals: 'timezone' and 'tzname'.
OffsetAbbr get_offset_abbr(const std::tm& tm) {
  const bool is_dst = tm.tm_isdst > 0;
  const int off = _timezone + (is_dst ? 60 * 60 : 0);
  const char* abbr = tzname[is_dst];
  return {off, abbr};
}
#else
//
// Returns an OffsetAbbr using std::tm fields with various spellings.
//
#if !defined(tm_gmtoff) && !defined(tm_zone)
template <typename T>
OffsetAbbr get_offset_abbr(const T& tm, decltype(&T::tm_gmtoff) = nullptr,
                           decltype(&T::tm_zone) = nullptr) {
  return {tm.tm_gmtoff, tm.tm_zone};
}
#endif  // !defined(tm_gmtoff) && !defined(tm_zone)
#if !defined(__tm_gmtoff) && !defined(__tm_zone)
template <typename T>
OffsetAbbr get_offset_abbr(const T& tm, decltype(&T::__tm_gmtoff) = nullptr,
                           decltype(&T::__tm_zone) = nullptr) {
  return {tm.__tm_gmtoff, tm.__tm_zone};
}
#endif  // !defined(__tm_gmtoff) && !defined(__tm_zone)
#endif

}  // namespace

TimeZoneLibC::TimeZoneLibC(const std::string& name)
    : local_(name == "localtime") {}

time_zone::absolute_lookup TimeZoneLibC::BreakTime(
    const time_point<seconds>& tp) const {
  time_zone::absolute_lookup al;
  std::time_t t = ToUnixSeconds(tp);
  std::tm tm;
  if (local_) {
#if defined(_WIN32) || defined(_WIN64)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::tie(al.offset, al.abbr) = get_offset_abbr(tm);
  } else {
#if defined(_WIN32) || defined(_WIN64)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    al.offset = 0;
    al.abbr = "UTC";
  }
  al.cs = civil_second(tm.tm_year + year_t{1900}, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec);
  al.is_dst = tm.tm_isdst > 0;
  return al;
}

time_zone::civil_lookup TimeZoneLibC::MakeTime(const civil_second& cs) const {
  time_zone::civil_lookup cl;
  std::time_t t;
  if (local_) {
    // Does not handle SKIPPED/AMBIGUOUS or huge years.
    std::tm tm;
    tm.tm_year = static_cast<int>(cs.year() - 1900);
    tm.tm_mon = cs.month() - 1;
    tm.tm_mday = cs.day();
    tm.tm_hour = cs.hour();
    tm.tm_min = cs.minute();
    tm.tm_sec = cs.second();
    tm.tm_isdst = -1;
    t = std::mktime(&tm);
  } else {
    t = cs - civil_second();
  }
  cl.kind = time_zone::civil_lookup::UNIQUE;
  cl.pre = cl.trans = cl.post = FromUnixSeconds(t);
  return cl;
}

bool TimeZoneLibC::NextTransition(const time_point<seconds>& tp,
                                  time_zone::civil_transition* trans) const {
  return false;
}

bool TimeZoneLibC::PrevTransition(const time_point<seconds>& tp,
                                  time_zone::civil_transition* trans) const {
  return false;
}

std::string TimeZoneLibC::Version() const {
  return std::string();  // unknown
}

std::string TimeZoneLibC::Description() const {
  return local_ ? "localtime" : "UTC";
}

}  // namespace cctz
}  // namespace time_internal
}  // namespace absl
