/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_DateObject_h_
#define vm_DateObject_h_

#include "js/Date.h"
#include "js/Value.h"
#include "vm/DateTime.h"
#include "vm/NativeObject.h"

namespace js {

class DateObject : public NativeObject {
  // Time in milliseconds since the (Unix) epoch.
  static const uint32_t UTC_TIME_SLOT = 0;

  // Raw time zone offset in seconds, i.e. without daylight saving adjustment,
  // of the current system zone.
  //
  // This value is exclusively used to verify the cached slots are still valid.
  //
  // It is NOT the return value of Date.prototype.getTimezoneOffset()!
  static const uint32_t UTC_TIME_ZONE_OFFSET_SLOT = 1;

  /*
   * Cached slots holding local properties of the date.
   * These are undefined until the first actual lookup occurs
   * and are reset to undefined whenever the date's time is modified.
   */
  static const uint32_t COMPONENTS_START_SLOT = 2;

  static const uint32_t LOCAL_TIME_SLOT = COMPONENTS_START_SLOT + 0;
  static const uint32_t LOCAL_YEAR_SLOT = COMPONENTS_START_SLOT + 1;
  static const uint32_t LOCAL_MONTH_SLOT = COMPONENTS_START_SLOT + 2;
  static const uint32_t LOCAL_DATE_SLOT = COMPONENTS_START_SLOT + 3;
  static const uint32_t LOCAL_DAY_SLOT = COMPONENTS_START_SLOT + 4;

  /*
   * Unlike the above slots that hold LocalTZA-adjusted component values,
   * LOCAL_SECONDS_INTO_YEAR_SLOT holds a composite value that can be used
   * to compute LocalTZA-adjusted hours, minutes, and seconds values.
   * Specifically, LOCAL_SECONDS_INTO_YEAR_SLOT holds the number of
   * LocalTZA-adjusted seconds into the year. Unix timestamps ignore leap
   * seconds, so recovering hours/minutes/seconds requires only trivial
   * division/modulus operations.
   */
  static const uint32_t LOCAL_SECONDS_INTO_YEAR_SLOT =
      COMPONENTS_START_SLOT + 5;

  static const uint32_t RESERVED_SLOTS = LOCAL_SECONDS_INTO_YEAR_SLOT + 1;

 public:
  static const JSClass class_;
  static const JSClass protoClass_;

  js::DateTimeInfo::ForceUTC forceUTC() const;

  JS::ClippedTime clippedTime() const {
    double t = getFixedSlot(UTC_TIME_SLOT).toDouble();
    JS::ClippedTime clipped = JS::TimeClip(t);
    MOZ_ASSERT(mozilla::NumbersAreIdentical(clipped.toDouble(), t));
    return clipped;
  }

  const js::Value& UTCTime() const { return getFixedSlot(UTC_TIME_SLOT); }
  const js::Value& localTime() const {
    return getReservedSlot(LOCAL_TIME_SLOT);
  }

  // Set UTC time to a given time and invalidate cached local time.
  void setUTCTime(JS::ClippedTime t);
  void setUTCTime(JS::ClippedTime t, MutableHandleValue vp);

  // Cache the local time, year, month, and so forth of the object.
  // If UTC time is not finite (e.g., NaN), the local time
  // slots will be set to the UTC time without conversion.
  void fillLocalTimeSlots();

  const js::Value& localYear() const {
    return getReservedSlot(LOCAL_YEAR_SLOT);
  }
  const js::Value& localMonth() const {
    return getReservedSlot(LOCAL_MONTH_SLOT);
  }
  const js::Value& localDate() const {
    return getReservedSlot(LOCAL_DATE_SLOT);
  }
  const js::Value& localDay() const { return getReservedSlot(LOCAL_DAY_SLOT); }

  const js::Value& localSecondsIntoYear() const {
    return getReservedSlot(LOCAL_SECONDS_INTO_YEAR_SLOT);
  }
};

}  // namespace js

#endif  // vm_DateObject_h_
