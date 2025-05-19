/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_TemporalUnit_h
#define builtin_temporal_TemporalUnit_h

#include "mozilla/Assertions.h"

#include <stdint.h>

#include "builtin/temporal/Crash.h"

namespace js::temporal {
enum class TemporalUnit {
  Auto,
  Year,
  Month,
  Week,
  Day,
  Hour,
  Minute,
  Second,
  Millisecond,
  Microsecond,
  Nanosecond
};

constexpr int64_t ToNanoseconds(TemporalUnit unit) {
  switch (unit) {
    case TemporalUnit::Day:
      return 86'400'000'000'000;
    case TemporalUnit::Hour:
      return 3'600'000'000'000;
    case TemporalUnit::Minute:
      return 60'000'000'000;
    case TemporalUnit::Second:
      return 1'000'000'000;
    case TemporalUnit::Millisecond:
      return 1'000'000;
    case TemporalUnit::Microsecond:
      return 1'000;
    case TemporalUnit::Nanosecond:
      return 1;

    case TemporalUnit::Auto:
    case TemporalUnit::Year:
    case TemporalUnit::Month:
    case TemporalUnit::Week:
      break;
  }
  JS_CONSTEXPR_CRASH("Unexpected temporal unit");
}

constexpr int64_t ToMicroseconds(TemporalUnit unit) {
  switch (unit) {
    case TemporalUnit::Day:
      return 86'400'000'000;
    case TemporalUnit::Hour:
      return 3'600'000'000;
    case TemporalUnit::Minute:
      return 60'000'000;
    case TemporalUnit::Second:
      return 1'000'000;
    case TemporalUnit::Millisecond:
      return 1'000;
    case TemporalUnit::Microsecond:
      return 1;

    case TemporalUnit::Auto:
    case TemporalUnit::Year:
    case TemporalUnit::Month:
    case TemporalUnit::Week:
    case TemporalUnit::Nanosecond:
      break;
  }
  JS_CONSTEXPR_CRASH("Unexpected temporal unit");
}

constexpr int64_t ToMilliseconds(TemporalUnit unit) {
  switch (unit) {
    case TemporalUnit::Day:
      return 86'400'000;
    case TemporalUnit::Hour:
      return 3'600'000;
    case TemporalUnit::Minute:
      return 60'000;
    case TemporalUnit::Second:
      return 1'000;
    case TemporalUnit::Millisecond:
      return 1;

    case TemporalUnit::Auto:
    case TemporalUnit::Year:
    case TemporalUnit::Month:
    case TemporalUnit::Week:
    case TemporalUnit::Microsecond:
    case TemporalUnit::Nanosecond:
      break;
  }
  JS_CONSTEXPR_CRASH("Unexpected temporal unit");
}

constexpr int64_t ToSeconds(TemporalUnit unit) {
  switch (unit) {
    case TemporalUnit::Day:
      return 86'400;
    case TemporalUnit::Hour:
      return 3'600;
    case TemporalUnit::Minute:
      return 60;
    case TemporalUnit::Second:
      return 1;

    case TemporalUnit::Auto:
    case TemporalUnit::Year:
    case TemporalUnit::Month:
    case TemporalUnit::Week:
    case TemporalUnit::Millisecond:
    case TemporalUnit::Microsecond:
    case TemporalUnit::Nanosecond:
      break;
  }
  JS_CONSTEXPR_CRASH("Unexpected temporal unit");
}

constexpr int64_t UnitsPerDay(TemporalUnit unit) {
  switch (unit) {
    case TemporalUnit::Day:
      return 1;
    case TemporalUnit::Hour:
      return 24;
    case TemporalUnit::Minute:
      return 1440;
    case TemporalUnit::Second:
      return 86'400;
    case TemporalUnit::Millisecond:
      return 86'400'000;
    case TemporalUnit::Microsecond:
      return 86'400'000'000;
    case TemporalUnit::Nanosecond:
      return 86'400'000'000'000;

    case TemporalUnit::Auto:
    case TemporalUnit::Year:
    case TemporalUnit::Month:
    case TemporalUnit::Week:
      break;
  }
  JS_CONSTEXPR_CRASH("Unexpected temporal unit");
}

constexpr const char* TemporalUnitToString(TemporalUnit unit) {
  switch (unit) {
    case TemporalUnit::Auto:
      return "auto";
    case TemporalUnit::Year:
      return "year";
    case TemporalUnit::Month:
      return "month";
    case TemporalUnit::Week:
      return "week";
    case TemporalUnit::Day:
      return "day";
    case TemporalUnit::Hour:
      return "hour";
    case TemporalUnit::Minute:
      return "minute";
    case TemporalUnit::Second:
      return "second";
    case TemporalUnit::Millisecond:
      return "millisecond";
    case TemporalUnit::Microsecond:
      return "microsecond";
    case TemporalUnit::Nanosecond:
      return "nanosecond";
  }
  JS_CONSTEXPR_CRASH("Unexpected temporal unit");
}

} /* namespace js::temporal */

#endif /* builtin_temporal_TemporalUnit_h */
