// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/time/civil_time.h"

#include <cstdlib>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/time/time.h"

namespace absl {

namespace {

// Since a civil time has a larger year range than absl::Time (64-bit years vs
// 64-bit seconds, respectively) we normalize years to roughly +/- 400 years
// around the year 2400, which will produce an equivalent year in a range that
// absl::Time can handle.
inline civil_year_t NormalizeYear(civil_year_t year) {
  return 2400 + year % 400;
}

// Formats the given CivilSecond according to the given format.
std::string FormatYearAnd(string_view fmt, CivilSecond cs) {
  const CivilSecond ncs(NormalizeYear(cs.year()), cs.month(), cs.day(),
                        cs.hour(), cs.minute(), cs.second());
  const TimeZone utc = UTCTimeZone();
  // TODO(absl-team): Avoid conversion of fmt std::string.
  return StrCat(cs.year(), FormatTime(std::string(fmt), FromCivil(ncs, utc), utc));
}

}  // namespace

std::string FormatCivilTime(CivilSecond c) {
  return FormatYearAnd("-%m-%dT%H:%M:%S", c);
}
std::string FormatCivilTime(CivilMinute c) {
  return FormatYearAnd("-%m-%dT%H:%M", c);
}
std::string FormatCivilTime(CivilHour c) {
  return FormatYearAnd("-%m-%dT%H", c);
}
std::string FormatCivilTime(CivilDay c) {
  return FormatYearAnd("-%m-%d", c);
}
std::string FormatCivilTime(CivilMonth c) {
  return FormatYearAnd("-%m", c);
}
std::string FormatCivilTime(CivilYear c) {
  return FormatYearAnd("", c);
}

namespace time_internal {

std::ostream& operator<<(std::ostream& os, CivilYear y) {
  return os << FormatCivilTime(y);
}
std::ostream& operator<<(std::ostream& os, CivilMonth m) {
  return os << FormatCivilTime(m);
}
std::ostream& operator<<(std::ostream& os, CivilDay d) {
  return os << FormatCivilTime(d);
}
std::ostream& operator<<(std::ostream& os, CivilHour h) {
  return os << FormatCivilTime(h);
}
std::ostream& operator<<(std::ostream& os, CivilMinute m) {
  return os << FormatCivilTime(m);
}
std::ostream& operator<<(std::ostream& os, CivilSecond s) {
  return os << FormatCivilTime(s);
}

}  // namespace time_internal

}  // namespace absl
