/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_ToString_h
#define builtin_temporal_ToString_h

#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalUnit.h"
#include "js/TypeDecls.h"

namespace js::temporal {

class CalendarValue;
class InstantObject;
class PlainDateObject;
class PlainMonthDayObject;
class PlainYearMonthObject;
class TimeZoneValue;
class ZonedDateTime;

struct EpochNanoseconds;
struct ISODateTime;
struct Time;

/**
 * TemporalInstantToString ( instant, timeZone, precision )
 */
JSString* TemporalInstantToString(JSContext* cx,
                                  const EpochNanoseconds& epochNs,
                                  JS::Handle<TimeZoneValue> timeZone,
                                  Precision precision);

/**
 * TemporalDateToString ( temporalDate, showCalendar )
 */
JSString* TemporalDateToString(JSContext* cx,
                               JS::Handle<PlainDateObject*> temporalDate,
                               ShowCalendar showCalendar);

/**
 * ISODateTimeToString ( isoDateTime, calendar, precision, showCalendar )
 */
JSString* ISODateTimeToString(JSContext* cx, const ISODateTime& isoDateTime,
                              JS::Handle<CalendarValue> calendar,
                              Precision precision, ShowCalendar showCalendar);

/**
 * TimeRecordToString ( time, precision )
 */
JSString* TimeRecordToString(JSContext* cx, const Time& time,
                             Precision precision);

/**
 * TemporalMonthDayToString ( monthDay, showCalendar )
 */
JSString* TemporalMonthDayToString(JSContext* cx,
                                   JS::Handle<PlainMonthDayObject*> monthDay,
                                   ShowCalendar showCalendar);

/**
 * TemporalYearMonthToString ( yearMonth, showCalendar )
 */
JSString* TemporalYearMonthToString(JSContext* cx,
                                    JS::Handle<PlainYearMonthObject*> yearMonth,
                                    ShowCalendar showCalendar);

/**
 * TemporalZonedDateTimeToString ( zonedDateTime, precision, showCalendar,
 * showTimeZone, showOffset [ , increment, unit, roundingMode ] )
 */
JSString* TemporalZonedDateTimeToString(
    JSContext* cx, JS::Handle<ZonedDateTime> zonedDateTime, Precision precision,
    ShowCalendar showCalendar, ShowTimeZoneName showTimeZone,
    ShowOffset showOffset, Increment increment = Increment{1},
    TemporalUnit unit = TemporalUnit::Nanosecond,
    TemporalRoundingMode roundingMode = TemporalRoundingMode::Trunc);

} /* namespace js::temporal */

#endif /* builtin_temporal_ToString_h */
