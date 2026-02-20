/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS Date class interface.
 */

#ifndef jsdate_h
#define jsdate_h

#include "jstypes.h"

#include "js/Date.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

namespace js {

/*
 * These functions provide a C interface to the date/time object
 */

/*
 * Construct a new Date Object from a time value given in milliseconds UTC
 * since the epoch.
 */
extern JSObject* NewDateObjectMsec(JSContext* cx, JS::ClippedTime t,
                                   JS::HandleObject proto = nullptr);

/*
 * Construct a new Date Object from an exploded local time value.
 *
 * Assert that mon < 12 to help catch off-by-one user errors, which are common
 * due to the 0-based month numbering copied into JS from Java (java.util.Date
 * in 1995).
 */
extern JS_PUBLIC_API JSObject* NewDateObject(JSContext* cx, int year, int mon,
                                             int mday, int hour, int min,
                                             int sec);

/*
 * Returns the current time in milliseconds since the epoch.
 */
JS::ClippedTime DateNow(JSContext* cx);

bool date_valueOf(JSContext* cx, unsigned argc, JS::Value* vp);

bool date_toPrimitive(JSContext* cx, unsigned argc, JS::Value* vp);

struct YearMonthDay {
  // Signed year in the range [-271821, 275760].
  int32_t year;

  // 0-indexed month, i.e. 0 is January, 1 is February, ..., 11 is December.
  int32_t month;

  // 1-indexed day of month.
  int32_t day;
};

/*
 * Split an epoch milliseconds value into year-month-day parts.
 */
YearMonthDay ToYearMonthDay(int64_t time);

struct HourMinuteSecond {
  // Hours from 0 to 23.
  int32_t hour;

  // Minutes from 0 to 59.
  int32_t minute;

  // Seconds from 0 to 59.
  int32_t second;
};

/*
 * Split an epoch milliseconds value into hour-minute-second parts.
 */
HourMinuteSecond ToHourMinuteSecond(int64_t epochMilliseconds);

} /* namespace js */

#endif /* jsdate_h */
