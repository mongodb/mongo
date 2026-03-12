/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_PlainYearMonth_h
#define builtin_temporal_PlainYearMonth_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stdint.h>

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/TemporalTypes.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/Value.h"
#include "vm/NativeObject.h"

namespace js {
struct ClassSpec;
}

namespace js::temporal {

class PlainYearMonthObject : public NativeObject {
 public:
  static const JSClass class_;
  static const JSClass& protoClass_;

  static constexpr uint32_t PACKED_DATE_SLOT = 0;
  static constexpr uint32_t CALENDAR_SLOT = 1;
  static constexpr uint32_t SLOT_COUNT = 2;

  /**
   * Extract the date fields from this PlainYearMonth object.
   */
  ISODate date() const {
    auto packed = PackedDate{getFixedSlot(PACKED_DATE_SLOT).toPrivateUint32()};
    return PackedDate::unpack(packed);
  }

  CalendarValue calendar() const {
    return CalendarValue(getFixedSlot(CALENDAR_SLOT));
  }

 private:
  static const ClassSpec classSpec_;
};

/**
 * ISOYearMonthWithinLimits ( isoDate )
 */
bool ISOYearMonthWithinLimits(const ISODate& isoDate);

class MOZ_STACK_CLASS PlainYearMonth final {
  ISODate date_;
  CalendarValue calendar_;

 public:
  PlainYearMonth() = default;

  PlainYearMonth(const ISODate& date, const CalendarValue& calendar)
      : date_(date), calendar_(calendar) {
    MOZ_ASSERT(ISOYearMonthWithinLimits(date));
  }

  explicit PlainYearMonth(const PlainYearMonthObject* yearMonth)
      : PlainYearMonth(yearMonth->date(), yearMonth->calendar()) {}

  const auto& date() const { return date_; }
  const auto& calendar() const { return calendar_; }

  // Allow implicit conversion to an ISODate.
  operator const ISODate&() const { return date(); }

  void trace(JSTracer* trc) { calendar_.trace(trc); }

  const auto* calendarDoNotUse() const { return &calendar_; }
};

/**
 * CreateTemporalYearMonth ( isoDate, calendar [ , newTarget ] )
 */
PlainYearMonthObject* CreateTemporalYearMonth(
    JSContext* cx, JS::Handle<PlainYearMonth> yearMonth);

/**
 * CreateTemporalYearMonth ( isoDate, calendar [ , newTarget ] )
 */
bool CreateTemporalYearMonth(JSContext* cx, const ISODate& isoDate,
                             JS::Handle<CalendarValue> calendar,
                             JS::MutableHandle<PlainYearMonth> result);

} /* namespace js::temporal */

namespace js {

template <typename Wrapper>
class WrappedPtrOperations<temporal::PlainYearMonth, Wrapper> {
  const auto& container() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  const auto& date() const { return container().date(); }

  JS::Handle<temporal::CalendarValue> calendar() const {
    return JS::Handle<temporal::CalendarValue>::fromMarkedLocation(
        container().calendarDoNotUse());
  }

  // Allow implicit conversion to an ISODate.
  operator const temporal::ISODate&() const { return date(); }
};

}  // namespace js

#endif /* builtin_temporal_PlainYearMonth_h */
