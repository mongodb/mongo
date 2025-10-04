/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_PlainYearMonth_h
#define builtin_temporal_PlainYearMonth_h

#include <stdint.h>

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/TemporalTypes.h"
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

  static constexpr uint32_t ISO_YEAR_SLOT = 0;
  static constexpr uint32_t ISO_MONTH_SLOT = 1;
  static constexpr uint32_t ISO_DAY_SLOT = 2;
  static constexpr uint32_t CALENDAR_SLOT = 3;
  static constexpr uint32_t SLOT_COUNT = 4;

  int32_t isoYear() const { return getFixedSlot(ISO_YEAR_SLOT).toInt32(); }

  int32_t isoMonth() const { return getFixedSlot(ISO_MONTH_SLOT).toInt32(); }

  int32_t isoDay() const { return getFixedSlot(ISO_DAY_SLOT).toInt32(); }

  CalendarValue calendar() const {
    return CalendarValue(getFixedSlot(CALENDAR_SLOT));
  }

 private:
  static const ClassSpec classSpec_;
};

/**
 * Extract the date fields from the PlainYearMonth object.
 */
inline PlainDate ToPlainDate(const PlainYearMonthObject* yearMonth) {
  return {yearMonth->isoYear(), yearMonth->isoMonth(), yearMonth->isoDay()};
}

/**
 * CreateTemporalYearMonth ( isoYear, isoMonth, calendar, referenceISODay [ ,
 * newTarget ] )
 */
PlainYearMonthObject* CreateTemporalYearMonth(
    JSContext* cx, const PlainDate& date, JS::Handle<CalendarValue> calendar);

} /* namespace js::temporal */

#endif /* builtin_temporal_PlainYearMonth_h */
