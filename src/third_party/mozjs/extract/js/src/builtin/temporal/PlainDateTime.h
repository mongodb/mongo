/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_PlainDateTime_h
#define builtin_temporal_PlainDateTime_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"

#include <stdint.h>

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/TemporalTypes.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

class JS_PUBLIC_API JSTracer;

namespace js {
struct ClassSpec;
}  // namespace js

namespace js::temporal {

class PlainDateTimeObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t PACKED_DATE_SLOT = 0;
  static constexpr uint32_t PACKED_TIME_SLOT = 1;
  static constexpr uint32_t CALENDAR_SLOT = 2;
  static constexpr uint32_t SLOT_COUNT = 3;

  /**
   * Extract the date fields from this PlainDateTime object.
   */
  ISODate date() const {
    auto packed = PackedDate{getFixedSlot(PACKED_DATE_SLOT).toPrivateUint32()};
    return PackedDate::unpack(packed);
  }

  /**
   * Extract the time fields from this PlainDateTime object.
   */
  Time time() const {
    auto packed = PackedTime{mozilla::BitwiseCast<uint64_t>(
        getFixedSlot(PACKED_TIME_SLOT).toDouble())};
    return PackedTime::unpack(packed);
  }

  /**
   * Extract the date-time fields from this PlainDateTime object.
   */
  ISODateTime dateTime() const { return {date(), time()}; }

  CalendarValue calendar() const {
    return CalendarValue(getFixedSlot(CALENDAR_SLOT));
  }

 private:
  static const ClassSpec classSpec_;
};

struct DifferenceSettings;
class Increment;
class CalendarFields;
enum class TemporalOverflow;
enum class TemporalRoundingMode;
enum class TemporalUnit;

#ifdef DEBUG
/**
 * IsValidISODate ( year, month, day )
 * IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond )
 */
bool IsValidISODateTime(const ISODateTime& isoDateTime);
#endif

/**
 * ISODateTimeWithinLimits ( isoDateTime )
 */
bool ISODateTimeWithinLimits(const ISODateTime& isoDateTime);

class MOZ_STACK_CLASS PlainDateTime final {
  ISODateTime dateTime_;
  CalendarValue calendar_;

 public:
  PlainDateTime() = default;

  PlainDateTime(const ISODateTime& dateTime, const CalendarValue& calendar)
      : dateTime_(dateTime), calendar_(calendar) {
    MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));
  }

  explicit PlainDateTime(const PlainDateTimeObject* dateTime)
      : PlainDateTime(dateTime->dateTime(), dateTime->calendar()) {}

  const auto& dateTime() const { return dateTime_; }
  const auto& date() const { return dateTime_.date; }
  const auto& time() const { return dateTime_.time; }
  const auto& calendar() const { return calendar_; }

  // Allow implicit conversion to an ISODateTime.
  operator const ISODateTime&() const { return dateTime(); }

  void trace(JSTracer* trc) { calendar_.trace(trc); }

  const auto* calendarDoNotUse() const { return &calendar_; }
};

/**
 * CreateTemporalDateTime ( isoDateTime, calendar [ , newTarget ] )
 */
PlainDateTimeObject* CreateTemporalDateTime(JSContext* cx,
                                            const ISODateTime& dateTime,
                                            JS::Handle<CalendarValue> calendar);

/**
 * InterpretTemporalDateTimeFields ( calendar, fields, overflow )
 */
bool InterpretTemporalDateTimeFields(JSContext* cx,
                                     JS::Handle<CalendarValue> calendar,
                                     JS::Handle<CalendarFields> fields,
                                     TemporalOverflow overflow,
                                     ISODateTime* result);

/**
 * RoundISODateTime ( isoDateTime, increment, unit, roundingMode )
 */
ISODateTime RoundISODateTime(const ISODateTime& isoDateTime,
                             Increment increment, TemporalUnit unit,
                             TemporalRoundingMode roundingMode);

/**
 * DifferencePlainDateTimeWithRounding ( isoDateTime1, isoDateTime2, calendar,
 * largestUnit, roundingIncrement, smallestUnit, roundingMode )
 */
bool DifferencePlainDateTimeWithRounding(JSContext* cx,
                                         const ISODateTime& isoDateTime1,
                                         const ISODateTime& isoDateTime2,
                                         JS::Handle<CalendarValue> calendar,
                                         const DifferenceSettings& settings,
                                         InternalDuration* result);
/**
 * DifferencePlainDateTimeWithTotal ( isoDateTime1, isoDateTime2, calendar, unit
 * )
 */
bool DifferencePlainDateTimeWithTotal(JSContext* cx,
                                      const ISODateTime& isoDateTime1,
                                      const ISODateTime& isoDateTime2,
                                      JS::Handle<CalendarValue> calendar,
                                      TemporalUnit unit, double* result);

} /* namespace js::temporal */

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::PlainDateTime, Wrapper> {
  const auto& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  const auto& dateTime() const { return container().dateTime(); }
  const auto& date() const { return container().date(); }
  const auto& time() const { return container().time(); }

  auto calendar() const {
    return JS::Handle<temporal::CalendarValue>::fromMarkedLocation(
        container().calendarDoNotUse());
  }

  // Allow implicit conversion to an ISODateTime.
  operator const temporal::ISODateTime&() const { return dateTime(); }
};

}  // namespace js

#endif /* builtin_temporal_PlainDateTime_h */
