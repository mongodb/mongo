/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/Calendar.h"

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/EnumSet.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/Range.h"
#include "mozilla/TextUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <stddef.h>
#include <stdint.h>
#include <utility>

#include "jsfriendapi.h"
#include "jsnum.h"
#include "jspubtd.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "builtin/Array.h"
#include "builtin/String.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainMonthDay.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/PlainYearMonth.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalFields.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/Wrapped.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "gc/AllocKind.h"
#include "gc/Barrier.h"
#include "gc/GCEnum.h"
#include "gc/Tracer.h"
#include "js/AllocPolicy.h"
#include "js/CallArgs.h"
#include "js/CallNonGenericMethod.h"
#include "js/Class.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/ForOfIterator.h"
#include "js/friend/ErrorMessages.h"
#include "js/GCAPI.h"
#include "js/GCHashTable.h"
#include "js/GCVector.h"
#include "js/Id.h"
#include "js/Printer.h"
#include "js/PropertyDescriptor.h"
#include "js/PropertySpec.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/Value.h"
#include "util/Text.h"
#include "vm/ArrayObject.h"
#include "vm/BytecodeUtil.h"
#include "vm/Compartment.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"
#include "vm/PropertyInfo.h"
#include "vm/PropertyKey.h"
#include "vm/Realm.h"
#include "vm/Shape.h"
#include "vm/Stack.h"
#include "vm/StringType.h"

#include "vm/Compartment-inl.h"
#include "vm/JSAtomUtils-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

static inline bool IsCalendar(Handle<Value> v) {
  return v.isObject() && v.toObject().is<CalendarObject>();
}

void js::temporal::CalendarValue::trace(JSTracer* trc) {
  TraceRoot(trc, &value_, "CalendarValue::value");
}

void js::temporal::CalendarRecord::trace(JSTracer* trc) {
  receiver_.trace(trc);
  TraceNullableRoot(trc, &dateAdd_, "CalendarRecord::dateAdd");
  TraceNullableRoot(trc, &dateFromFields_, "CalendarRecord::dateFromFields");
  TraceNullableRoot(trc, &dateUntil_, "CalendarRecord::dateUntil");
  TraceNullableRoot(trc, &day_, "CalendarRecord::day");
  TraceNullableRoot(trc, &fields_, "CalendarRecord::fields");
  TraceNullableRoot(trc, &mergeFields_, "CalendarRecord::mergeFields");
  TraceNullableRoot(trc, &monthDayFromFields_,
                    "CalendarRecord::monthDayFromFields");
  TraceNullableRoot(trc, &yearMonthFromFields_,
                    "CalendarRecord::yearMonthFromFields");
}

bool js::temporal::WrapCalendarValue(JSContext* cx,
                                     MutableHandle<JS::Value> calendar) {
  MOZ_ASSERT(calendar.isInt32() || calendar.isObject());
  return cx->compartment()->wrap(cx, calendar);
}

/**
 * IteratorToListOfType ( iteratorRecord, elementTypes )
 *
 * With `elementTypes = « String »`.
 *
 * This implementation accepts an iterable instead of an iterator record.
 */
static bool IterableToListOfStrings(JSContext* cx, Handle<Value> items,
                                    MutableHandle<CalendarFieldNames> list) {
  JS::ForOfIterator iterator(cx);
  if (!iterator.init(items)) {
    return false;
  }

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  Rooted<Value> nextValue(cx);
  Rooted<PropertyKey> value(cx);
  while (true) {
    // Step 2.a.
    bool done;
    if (!iterator.next(&nextValue, &done)) {
      return false;
    }

    // Step 2.b.
    if (done) {
      return true;
    }

    // Step 2.d. (Reordered)
    if (nextValue.isString()) {
      if (!PrimitiveValueToId<CanGC>(cx, nextValue, &value)) {
        return false;
      }
      if (!list.append(value)) {
        return false;
      }
      continue;
    }

    // Step 2.c.1.
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, nextValue,
                     nullptr, "not a string");

    // Step 2.c.2.
    iterator.closeThrow();
    return false;
  }
}

/**
 * IsISOLeapYear ( year )
 */
static constexpr bool IsISOLeapYear(int32_t year) {
  // Steps 1-5.
  return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

/**
 * IsISOLeapYear ( year )
 */
static bool IsISOLeapYear(double year) {
  // Step 1.
  MOZ_ASSERT(IsInteger(year));

  // Steps 2-5.
  return std::fmod(year, 4) == 0 &&
         (std::fmod(year, 100) != 0 || std::fmod(year, 400) == 0);
}

/**
 * ISODaysInYear ( year )
 */
int32_t js::temporal::ISODaysInYear(int32_t year) {
  // Steps 1-3.
  return IsISOLeapYear(year) ? 366 : 365;
}

/**
 * ISODaysInMonth ( year, month )
 */
static constexpr int32_t ISODaysInMonth(int32_t year, int32_t month) {
  MOZ_ASSERT(1 <= month && month <= 12);

  constexpr uint8_t daysInMonth[2][13] = {
      {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
      {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};

  // Steps 1-4.
  return daysInMonth[IsISOLeapYear(year)][month];
}

/**
 * ISODaysInMonth ( year, month )
 */
int32_t js::temporal::ISODaysInMonth(int32_t year, int32_t month) {
  return ::ISODaysInMonth(year, month);
}

/**
 * ISODaysInMonth ( year, month )
 */
int32_t js::temporal::ISODaysInMonth(double year, int32_t month) {
  MOZ_ASSERT(1 <= month && month <= 12);

  static constexpr uint8_t daysInMonth[2][13] = {
      {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
      {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};

  // Steps 1-4.
  return daysInMonth[IsISOLeapYear(year)][month];
}

/**
 * 21.4.1.6 Week Day
 *
 * Compute the week day from |day| without first expanding |day| into a full
 * date through |MakeDate(day, 0)|:
 *
 *   WeekDay(MakeDate(day, 0))
 * = WeekDay(day × msPerDay + 0)
 * = WeekDay(day × msPerDay)
 * = 𝔽(ℝ(Day(day × msPerDay) + 4𝔽) modulo 7)
 * = 𝔽(ℝ(𝔽(floor(ℝ((day × msPerDay) / msPerDay))) + 4𝔽) modulo 7)
 * = 𝔽(ℝ(𝔽(floor(ℝ(day))) + 4𝔽) modulo 7)
 * = 𝔽(ℝ(𝔽(day) + 4𝔽) modulo 7)
 */
static int32_t WeekDay(int32_t day) {
  int32_t result = (day + 4) % 7;
  if (result < 0) {
    result += 7;
  }
  return result;
}

/**
 * ToISODayOfWeek ( year, month, day )
 */
static int32_t ToISODayOfWeek(const PlainDate& date) {
  MOZ_ASSERT(ISODateTimeWithinLimits(date));

  // Steps 1-3. (Not applicable in our implementation.)

  // TODO: Check if ES MakeDate + WeekDay is efficient enough.
  //
  // https://en.wikipedia.org/wiki/Determination_of_the_day_of_the_week#Methods_in_computer_code

  // Step 4.
  int32_t day = MakeDay(date);

  // Step 5.
  int32_t weekday = WeekDay(day);
  return weekday != 0 ? weekday : 7;
}

static constexpr auto FirstDayOfMonth(int32_t year) {
  // The following array contains the day of year for the first day of each
  // month, where index 0 is January, and day 0 is January 1.
  std::array<int32_t, 13> days = {};
  for (int32_t month = 1; month <= 12; ++month) {
    days[month] = days[month - 1] + ::ISODaysInMonth(year, month);
  }
  return days;
}

/**
 * ToISODayOfYear ( year, month, day )
 */
static int32_t ToISODayOfYear(int32_t year, int32_t month, int32_t day) {
  MOZ_ASSERT(1 <= month && month <= 12);

  // First day of month arrays for non-leap and leap years.
  constexpr decltype(FirstDayOfMonth(0)) firstDayOfMonth[2] = {
      FirstDayOfMonth(1), FirstDayOfMonth(0)};

  // Steps 1-3. (Not applicable in our implementation.)

  // Steps 4-5.
  //
  // Instead of first computing the date and then using DayWithinYear to map the
  // date to the day within the year, directly lookup the first day of the month
  // and then add the additional days.
  return firstDayOfMonth[IsISOLeapYear(year)][month - 1] + day;
}

/**
 * ToISODayOfYear ( year, month, day )
 */
int32_t js::temporal::ToISODayOfYear(const PlainDate& date) {
  MOZ_ASSERT(ISODateTimeWithinLimits(date));

  // Steps 1-5.
  const auto& [year, month, day] = date;
  return ::ToISODayOfYear(year, month, day);
}

static int32_t FloorDiv(int32_t dividend, int32_t divisor) {
  MOZ_ASSERT(divisor > 0);

  int32_t quotient = dividend / divisor;
  int32_t remainder = dividend % divisor;
  if (remainder < 0) {
    quotient -= 1;
  }
  return quotient;
}

/**
 * 21.4.1.3 Year Number, DayFromYear
 */
static int32_t DayFromYear(int32_t year) {
  return 365 * (year - 1970) + FloorDiv(year - 1969, 4) -
         FloorDiv(year - 1901, 100) + FloorDiv(year - 1601, 400);
}

/**
 * 21.4.1.11 MakeTime ( hour, min, sec, ms )
 */
static int64_t MakeTime(const PlainTime& time) {
  MOZ_ASSERT(IsValidTime(time));

  // Step 1 (Not applicable).

  // Step 2.
  int64_t h = time.hour;

  // Step 3.
  int64_t m = time.minute;

  // Step 4.
  int64_t s = time.second;

  // Step 5.
  int64_t milli = time.millisecond;

  // Steps 6-7.
  return h * ToMilliseconds(TemporalUnit::Hour) +
         m * ToMilliseconds(TemporalUnit::Minute) +
         s * ToMilliseconds(TemporalUnit::Second) + milli;
}

/**
 * 21.4.1.12 MakeDay ( year, month, date )
 */
int32_t js::temporal::MakeDay(const PlainDate& date) {
  MOZ_ASSERT(ISODateTimeWithinLimits(date));

  return DayFromYear(date.year) + ToISODayOfYear(date) - 1;
}

/**
 * 21.4.1.13 MakeDate ( day, time )
 */
int64_t js::temporal::MakeDate(const PlainDateTime& dateTime) {
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));

  // Step 1 (Not applicable).

  // Steps 2-3.
  int64_t tv = MakeDay(dateTime.date) * ToMilliseconds(TemporalUnit::Day) +
               MakeTime(dateTime.time);

  // Step 4.
  return tv;
}

/**
 * 21.4.1.12 MakeDay ( year, month, date )
 */
static int32_t MakeDay(int32_t year, int32_t month, int32_t day) {
  MOZ_ASSERT(1 <= month && month <= 12);

  // FIXME: spec issue - what should happen for invalid years/days?
  return DayFromYear(year) + ::ToISODayOfYear(year, month, day) - 1;
}

/**
 * 21.4.1.13 MakeDate ( day, time )
 */
int64_t js::temporal::MakeDate(int32_t year, int32_t month, int32_t day) {
  // NOTE: This version accepts values outside the valid date-time limits.
  MOZ_ASSERT(1 <= month && month <= 12);

  // Step 1 (Not applicable).

  // Steps 2-3.
  int64_t tv = ::MakeDay(year, month, day) * ToMilliseconds(TemporalUnit::Day);

  // Step 4.
  return tv;
}

struct YearWeek final {
  int32_t year = 0;
  int32_t week = 0;
};

/**
 * ToISOWeekOfYear ( year, month, day )
 */
static YearWeek ToISOWeekOfYear(const PlainDate& date) {
  MOZ_ASSERT(ISODateTimeWithinLimits(date));

  const auto& [year, month, day] = date;

  // TODO: https://en.wikipedia.org/wiki/Week#The_ISO_week_date_system
  // TODO: https://en.wikipedia.org/wiki/ISO_week_date#Algorithms

  // Steps 1-3. (Not applicable in our implementation.)

  // Steps 4-5.
  int32_t doy = ToISODayOfYear(date);
  int32_t dow = ToISODayOfWeek(date);

  int32_t woy = (10 + doy - dow) / 7;
  MOZ_ASSERT(0 <= woy && woy <= 53);

  // An ISO year has 53 weeks if the year starts on a Thursday or if it's a
  // leap year which starts on a Wednesday.
  auto isLongYear = [](int32_t year) {
    int32_t startOfYear = ToISODayOfWeek({year, 1, 1});
    return startOfYear == 4 || (startOfYear == 3 && IsISOLeapYear(year));
  };

  // Part of last year's last week, which is either week 52 or week 53.
  if (woy == 0) {
    return {year - 1, 52 + int32_t(isLongYear(year - 1))};
  }

  // Part of next year's first week if the current year isn't a long year.
  if (woy == 53 && !isLongYear(year)) {
    return {year + 1, 1};
  }

  return {year, woy};
}

/**
 * ISOMonthCode ( month )
 */
static JSString* ISOMonthCode(JSContext* cx, int32_t month) {
  MOZ_ASSERT(1 <= month && month <= 12);

  // Steps 1-2.
  char monthCode[3] = {'M', char('0' + (month / 10)), char('0' + (month % 10))};
  return NewStringCopyN<CanGC>(cx, monthCode, std::size(monthCode));
}

template <typename T, typename... Ts>
static bool ToPlainDate(JSObject* temporalDateLike, PlainDate* result) {
  if (auto* obj = temporalDateLike->maybeUnwrapIf<T>()) {
    *result = ToPlainDate(obj);
    return true;
  }
  if constexpr (sizeof...(Ts) > 0) {
    return ToPlainDate<Ts...>(temporalDateLike, result);
  }
  return false;
}

template <typename... Ts>
static bool ToPlainDate(JSContext* cx, Handle<Value> temporalDateLike,
                        PlainDate* result) {
  if (temporalDateLike.isObject()) {
    if (ToPlainDate<Ts...>(&temporalDateLike.toObject(), result)) {
      return true;
    }
  }

  return ToTemporalDate(cx, temporalDateLike, result);
}

#ifdef DEBUG
template <typename CharT>
static bool StringIsAsciiLowerCase(mozilla::Range<CharT> str) {
  return std::all_of(str.begin().get(), str.end().get(), [](CharT ch) {
    return mozilla::IsAscii(ch) && !mozilla::IsAsciiUppercaseAlpha(ch);
  });
}

static bool StringIsAsciiLowerCase(const JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  return str->hasLatin1Chars()
             ? StringIsAsciiLowerCase(str->latin1Range(nogc))
             : StringIsAsciiLowerCase(str->twoByteRange(nogc));
}
#endif

/**
 * Return the BCP-47 string for the given calendar id.
 */
static const char* CalendarIdToBcp47(CalendarId id) {
  switch (id) {
    case CalendarId::ISO8601:
      return "iso8601";
  }
  MOZ_CRASH("invalid calendar id");
}

/**
 * AvailableCalendars ( )
 */
static constexpr auto AvailableCalendars() {
  return std::array{
      CalendarId::ISO8601,
  };
}

/**
 * IsBuiltinCalendar ( id )
 */
static mozilla::Maybe<CalendarId> IsBuiltinCalendar(JSLinearString* id) {
  // Callers must convert to lower case.
  MOZ_ASSERT(StringIsAsciiLowerCase(id));

  // Step 1.
  static constexpr auto calendars = AvailableCalendars();

  // Step 2.
  for (auto identifier : calendars) {
    if (StringEqualsAscii(id, CalendarIdToBcp47(identifier))) {
      return mozilla::Some(identifier);
    }
  }

  // Step 3.
  return mozilla::Nothing();
}

static bool ToBuiltinCalendar(JSContext* cx, Handle<JSLinearString*> id,
                              CalendarId* result) {
  if (!StringIsAscii(id)) {
    if (auto chars = QuoteString(cx, id)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_CALENDAR_INVALID_ID, chars.get());
    }
    return false;
  }

  JSString* lower = StringToLowerCase(cx, id);
  if (!lower) {
    return false;
  }

  JSLinearString* linear = lower->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  auto builtin = IsBuiltinCalendar(linear);
  if (!builtin) {
    if (auto chars = QuoteString(cx, id)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_CALENDAR_INVALID_ID, chars.get());
    }
    return false;
  }

  *result = *builtin;
  return true;
}

bool js::temporal::ToBuiltinCalendar(JSContext* cx, Handle<JSString*> id,
                                     MutableHandle<CalendarValue> result) {
  Rooted<JSLinearString*> linear(cx, id->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  CalendarId identifier;
  if (!::ToBuiltinCalendar(cx, linear, &identifier)) {
    return false;
  }

  result.set(CalendarValue(identifier));
  return true;
}

/**
 * CreateTemporalCalendar ( identifier [ , newTarget ] )
 */
static CalendarObject* CreateTemporalCalendar(JSContext* cx,
                                              const CallArgs& args,
                                              CalendarId identifier) {
  // Step 1. (Not applicable in our implementation.)

  // Steps 2-3.
  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Calendar, &proto)) {
    return nullptr;
  }

  auto* obj = NewObjectWithClassProto<CalendarObject>(cx, proto);
  if (!obj) {
    return nullptr;
  }

  // Step 4.
  obj->setFixedSlot(CalendarObject::IDENTIFIER_SLOT,
                    Int32Value(static_cast<int32_t>(identifier)));

  // Step 5.
  return obj;
}

/**
 * CreateTemporalCalendar ( identifier [ , newTarget ] )
 */
static CalendarObject* CreateTemporalCalendar(JSContext* cx,
                                              CalendarId identifier) {
  // Step 1. (Not applicable in our implementation.)

  // Steps 2-3.
  auto* obj = NewBuiltinClassInstance<CalendarObject>(cx);
  if (!obj) {
    return nullptr;
  }

  // Step 4.
  obj->setFixedSlot(CalendarObject::IDENTIFIER_SLOT,
                    Int32Value(static_cast<int32_t>(identifier)));

  // Step 5.
  return obj;
}

/**
 * ObjectImplementsTemporalCalendarProtocol ( object )
 */
static bool ObjectImplementsTemporalCalendarProtocol(JSContext* cx,
                                                     Handle<JSObject*> object,
                                                     bool* result) {
  // Step 1. (Not applicable in our implementation.)
  MOZ_ASSERT(!object->canUnwrapAs<CalendarObject>(),
             "Calendar objects handled in the caller");

  // Step 2.
  for (auto key : {
           &JSAtomState::dateAdd,      &JSAtomState::dateFromFields,
           &JSAtomState::dateUntil,    &JSAtomState::day,
           &JSAtomState::dayOfWeek,    &JSAtomState::dayOfYear,
           &JSAtomState::daysInMonth,  &JSAtomState::daysInWeek,
           &JSAtomState::daysInYear,   &JSAtomState::fields,
           &JSAtomState::id,           &JSAtomState::inLeapYear,
           &JSAtomState::mergeFields,  &JSAtomState::month,
           &JSAtomState::monthCode,    &JSAtomState::monthDayFromFields,
           &JSAtomState::monthsInYear, &JSAtomState::weekOfYear,
           &JSAtomState::year,         &JSAtomState::yearMonthFromFields,
           &JSAtomState::yearOfWeek,
       }) {
    // Step 2.a.
    bool has;
    if (!HasProperty(cx, object, cx->names().*key, &has)) {
      return false;
    }
    if (!has) {
      *result = false;
      return true;
    }
  }

  // Step 3.
  *result = true;
  return true;
}

template <typename T, typename... Ts>
static bool ToTemporalCalendar(JSContext* cx, Handle<JSObject*> object,
                               MutableHandle<CalendarValue> result) {
  if (auto* unwrapped = object->maybeUnwrapIf<T>()) {
    result.set(unwrapped->calendar());
    return result.wrap(cx);
  }

  if constexpr (sizeof...(Ts) > 0) {
    return ToTemporalCalendar<Ts...>(cx, object, result);
  }

  result.set(CalendarValue());
  return true;
}

/**
 * ToTemporalCalendarSlotValue ( temporalCalendarLike [ , default ] )
 */
bool js::temporal::ToTemporalCalendar(JSContext* cx,
                                      Handle<Value> temporalCalendarLike,
                                      MutableHandle<CalendarValue> result) {
  // Step 1. (Not applicable)

  // Step 2.
  Rooted<Value> calendarLike(cx, temporalCalendarLike);
  if (calendarLike.isObject()) {
    Rooted<JSObject*> obj(cx, &calendarLike.toObject());

    // Step 2.b. (Partial)
    if (obj->canUnwrapAs<CalendarObject>()) {
      result.set(CalendarValue(obj));
      return true;
    }

    // Step 2.a.
    Rooted<CalendarValue> calendar(cx);
    if (!::ToTemporalCalendar<PlainDateObject, PlainDateTimeObject,
                              PlainMonthDayObject, PlainYearMonthObject,
                              ZonedDateTimeObject>(cx, obj, &calendar)) {
      return false;
    }
    if (calendar) {
      result.set(calendar);
      return true;
    }

    // Step 2.b.
    bool implementsCalendarProtocol;
    if (!ObjectImplementsTemporalCalendarProtocol(
            cx, obj, &implementsCalendarProtocol)) {
      return false;
    }
    if (!implementsCalendarProtocol) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_INVALID_OBJECT,
                               "Temporal.Calendar", obj->getClass()->name);
      return false;
    }

    // Step 2.c.
    result.set(CalendarValue(obj));
    return true;
  }

  // Step 3.
  if (!calendarLike.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK,
                     calendarLike, nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> str(cx, calendarLike.toString());

  // Step 4.
  Rooted<JSLinearString*> id(cx, ParseTemporalCalendarString(cx, str));
  if (!id) {
    return false;
  }

  // Step 5.
  CalendarId identifier;
  if (!::ToBuiltinCalendar(cx, id, &identifier)) {
    return false;
  }

  // Step 6.
  result.set(CalendarValue(identifier));
  return true;
}

/**
 * ToTemporalCalendarSlotValue ( temporalCalendarLike [ , default ] )
 *
 * When called with `default = "iso8601"`.
 */
bool js::temporal::ToTemporalCalendarWithISODefault(
    JSContext* cx, Handle<Value> temporalCalendarLike,
    MutableHandle<CalendarValue> result) {
  // Step 1.
  if (temporalCalendarLike.isUndefined()) {
    result.set(CalendarValue(CalendarId::ISO8601));
    return true;
  }

  // Steps 2-6.
  return ToTemporalCalendar(cx, temporalCalendarLike, result);
}

/**
 * GetTemporalCalendarSlotValueWithISODefault ( item )
 */
bool js::temporal::GetTemporalCalendarWithISODefault(
    JSContext* cx, Handle<JSObject*> item,
    MutableHandle<CalendarValue> result) {
  // Step 1.
  Rooted<CalendarValue> calendar(cx);
  if (!::ToTemporalCalendar<PlainDateObject, PlainDateTimeObject,
                            PlainMonthDayObject, PlainYearMonthObject,
                            ZonedDateTimeObject>(cx, item, &calendar)) {
    return false;
  }
  if (calendar) {
    result.set(calendar);
    return true;
  }

  // Step 2.
  Rooted<Value> calendarValue(cx);
  if (!GetProperty(cx, item, item, cx->names().calendar, &calendarValue)) {
    return false;
  }

  // Step 3.
  return ToTemporalCalendarWithISODefault(cx, calendarValue, result);
}

/**
 * ToTemporalCalendarIdentifier ( calendarSlotValue )
 */
static JSLinearString* ToTemporalCalendarIdentifier(JSContext* cx,
                                                    CalendarId id) {
  // TODO: Avoid string allocations?
  return NewStringCopyZ<CanGC>(cx, CalendarIdToBcp47(id));
}

/**
 * ToTemporalCalendarIdentifier ( calendarSlotValue )
 */
JSLinearString* js::temporal::ToTemporalCalendarIdentifier(
    JSContext* cx, Handle<CalendarValue> calendar) {
  // Step 1.
  if (calendar.isString()) {
    return ToTemporalCalendarIdentifier(cx, calendar.toString());
  }

  // Step 2.
  Rooted<JSObject*> calendarObj(cx, calendar.toObject());
  Rooted<Value> identifier(cx);
  if (!GetProperty(cx, calendarObj, calendarObj, cx->names().id, &identifier)) {
    return nullptr;
  }

  // Step 3.
  if (!identifier.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, identifier,
                     nullptr, "not a string");
    return nullptr;
  }

  // Step 4.
  return identifier.toString()->ensureLinear(cx);
}

/**
 * ToTemporalCalendarObject ( calendarSlotValue )
 */
JSObject* js::temporal::ToTemporalCalendarObject(
    JSContext* cx, Handle<CalendarValue> calendar) {
  // Step 1.
  if (calendar.isObject()) {
    return calendar.toObject();
  }

  // Step 2.
  return CreateTemporalCalendar(cx, calendar.toString());
}

bool js::temporal::ToTemporalCalendar(JSContext* cx,
                                      const CalendarValue& calendar,
                                      MutableHandle<Value> result) {
  if (calendar.isObject()) {
    result.setObject(*calendar.toObject());
    return true;
  }

  auto* str = ToTemporalCalendarIdentifier(cx, calendar.toString());
  if (!str) {
    return false;
  }

  result.setString(str);
  return true;
}

static bool Calendar_dateAdd(JSContext* cx, unsigned argc, Value* vp);
static bool Calendar_dateFromFields(JSContext* cx, unsigned argc, Value* vp);
static bool Calendar_dateUntil(JSContext* cx, unsigned argc, Value* vp);
static bool Calendar_day(JSContext* cx, unsigned argc, Value* vp);
static bool Calendar_fields(JSContext* cx, unsigned argc, Value* vp);
static bool Calendar_mergeFields(JSContext* cx, unsigned argc, Value* vp);
static bool Calendar_monthDayFromFields(JSContext* cx, unsigned argc,
                                        Value* vp);
static bool Calendar_yearMonthFromFields(JSContext* cx, unsigned argc,
                                         Value* vp);

/**
 * CalendarMethodsRecordLookup ( calendarRec, methodName )
 */
static bool CalendarMethodsRecordLookup(JSContext* cx,
                                        MutableHandle<CalendarRecord> calendar,
                                        CalendarMethod methodName) {
  // Step 1. (Not applicable in our implementation.)

  // Steps 2-10.
  Rooted<JSObject*> object(cx, calendar.receiver().toObject());

  auto lookup = [&](Handle<PropertyName*> name, JSNative native,
                    MutableHandle<JSObject*> result) {
    auto* method = GetMethod(cx, object, name);
    if (!method) {
      return false;
    }

    // As an optimization we only store the method if the receiver is either
    // a custom calendar object or if the method isn't the default, built-in
    // calender method.
    if (!object->is<CalendarObject>() || !IsNativeFunction(method, native)) {
      result.set(method);
    }
    return true;
  };

  switch (methodName) {
    // Steps 2 and 10.
    case CalendarMethod::DateAdd:
      return lookup(cx->names().dateAdd, Calendar_dateAdd, calendar.dateAdd());

      // Steps 3 and 10.
    case CalendarMethod::DateFromFields:
      return lookup(cx->names().dateFromFields, Calendar_dateFromFields,
                    calendar.dateFromFields());

      // Steps 4 and 10.
    case CalendarMethod::DateUntil:
      return lookup(cx->names().dateUntil, Calendar_dateUntil,
                    calendar.dateUntil());

      // Steps 5 and 10.
    case CalendarMethod::Day:
      return lookup(cx->names().day, Calendar_day, calendar.day());

      // Steps 6 and 10.
    case CalendarMethod::Fields:
      return lookup(cx->names().fields, Calendar_fields, calendar.fields());

      // Steps 7 and 10.
    case CalendarMethod::MergeFields:
      return lookup(cx->names().mergeFields, Calendar_mergeFields,
                    calendar.mergeFields());

      // Steps 8 and 10.
    case CalendarMethod::MonthDayFromFields:
      return lookup(cx->names().monthDayFromFields, Calendar_monthDayFromFields,
                    calendar.monthDayFromFields());

      // Steps 9 and 10.
    case CalendarMethod::YearMonthFromFields:
      return lookup(cx->names().yearMonthFromFields,
                    Calendar_yearMonthFromFields,
                    calendar.yearMonthFromFields());
  }

  MOZ_CRASH("invalid calendar method");
}

/**
 * CreateCalendarMethodsRecord ( calendar, methods )
 */
bool js::temporal::CreateCalendarMethodsRecord(
    JSContext* cx, Handle<CalendarValue> calendar,
    mozilla::EnumSet<CalendarMethod> methods,
    MutableHandle<CalendarRecord> result) {
  MOZ_ASSERT(!methods.isEmpty());

  // Step 1.
  result.set(CalendarRecord{calendar});

#ifdef DEBUG
  // Remember the set of looked-up methods for assertions.
  result.get().lookedUp() += methods;
#endif

  // Built-in calendars don't perform observable lookups.
  if (calendar.isString()) {
    return true;
  }

  // Step 2.
  for (auto method : methods) {
    if (!CalendarMethodsRecordLookup(cx, result, method)) {
      return false;
    }
  }

  // Step 3.
  return true;
}

static CalendarId BuiltinCalendarId(const CalendarValue& calendar) {
  if (calendar.isString()) {
    return calendar.toString();
  }
  return calendar.toObject()->as<CalendarObject>().identifier();
}

static bool ToCalendarField(JSContext* cx, JSLinearString* linear,
                            CalendarField* result) {
  if (StringEqualsLiteral(linear, "year")) {
    *result = CalendarField::Year;
    return true;
  }
  if (StringEqualsLiteral(linear, "month")) {
    *result = CalendarField::Month;
    return true;
  }
  if (StringEqualsLiteral(linear, "monthCode")) {
    *result = CalendarField::MonthCode;
    return true;
  }
  if (StringEqualsLiteral(linear, "day")) {
    *result = CalendarField::Day;
    return true;
  }
  if (auto chars = QuoteString(cx, linear, '"')) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_TEMPORAL_CALENDAR_INVALID_FIELD,
                             chars.get());
  }
  return false;
}

static constexpr auto sortedCalendarFields = std::array{
    CalendarField::Day,
    CalendarField::Month,
    CalendarField::MonthCode,
    CalendarField::Year,
};

// TODO: Consider reordering CalendarField so we don't need this. Probably best
// to decide after <https://github.com/tc39/proposal-temporal/issues/2826> has
// landed.
using SortedCalendarFields = SortedEnumSet<CalendarField, sortedCalendarFields>;

static TemporalField ToTemporalField(CalendarField field) {
  switch (field) {
    case CalendarField::Year:
      return TemporalField::Year;
    case CalendarField::Month:
      return TemporalField::Month;
    case CalendarField::MonthCode:
      return TemporalField::MonthCode;
    case CalendarField::Day:
      return TemporalField::Day;
  }
  MOZ_CRASH("invalid calendar field name");
}

/**
 * Temporal.Calendar.prototype.fields ( fields )
 */
static bool BuiltinCalendarFields(JSContext* cx, CalendarId calendarId,
                                  mozilla::EnumSet<CalendarField> fieldNames,
                                  CalendarFieldNames& result) {
  MOZ_ASSERT(result.empty());

  // Steps 1-4. (Not applicable.)

  // Steps 5-6.
  mozilla::EnumSet<TemporalField> temporalFields{};
  for (auto fieldName : fieldNames) {
    // Steps 6.a and 6.b.i-iii. (Not applicable)

    // Step 6.b.iv.
    temporalFields += ToTemporalField(fieldName);
  }

  // Reserve space for the append operation.
  if (!result.reserve(temporalFields.size())) {
    return false;
  }

  // Append all fields, sorted.
  for (auto field : SortedTemporalFields{temporalFields}) {
    auto* name = ToPropertyName(cx, field);
    result.infallibleAppend(NameToId(name));
  }

  return true;
}

/**
 * Temporal.Calendar.prototype.fields ( fields )
 */
static bool BuiltinCalendarFields(JSContext* cx, CalendarId calendarId,
                                  Handle<Value> fields,
                                  MutableHandle<Value> result) {
  // Step 3.
  JS::ForOfIterator iterator(cx);
  if (!iterator.init(fields)) {
    return false;
  }

  // Step 4.
  JS::RootedVector<Value> fieldNames(cx);
  mozilla::EnumSet<CalendarField> seen;

  // Steps 5-6.
  Rooted<Value> nextValue(cx);
  Rooted<JSLinearString*> linear(cx);
  while (true) {
    // Step 6.a.
    bool done;
    if (!iterator.next(&nextValue, &done)) {
      return false;
    }
    if (done) {
      break;
    }

    // Step 6.b.i.
    if (!nextValue.isString()) {
      // Step 6.b.i.1.
      ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, nextValue,
                       nullptr, "not a string");

      // Step 6.b.i.2.
      iterator.closeThrow();
      return false;
    }

    linear = nextValue.toString()->ensureLinear(cx);
    if (!linear) {
      return false;
    }

    // Step 6.b.iii. (Reordered)
    CalendarField field;
    if (!ToCalendarField(cx, linear, &field)) {
      iterator.closeThrow();
      return false;
    }

    // Step 6.b.ii.
    if (seen.contains(field)) {
      // Step 6.b.ii.1.
      if (auto chars = QuoteString(cx, linear, '"')) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_TEMPORAL_CALENDAR_DUPLICATE_FIELD,
                                 chars.get());
      }

      // Step 6.b.ii.2.
      iterator.closeThrow();
      return false;
    }

    // Step 6.b.iv.
    if (!fieldNames.append(nextValue)) {
      return false;
    }
    seen += field;
  }

  // Steps 7-9.
  auto* array =
      NewDenseCopiedArray(cx, fieldNames.length(), fieldNames.begin());
  if (!array) {
    return false;
  }

  result.setObject(*array);
  return true;
}

/**
 * CalendarFields ( calendarRec, fieldNames )
 */
bool js::temporal::CalendarFields(JSContext* cx,
                                  Handle<CalendarRecord> calendar,
                                  mozilla::EnumSet<CalendarField> fieldNames,
                                  MutableHandle<CalendarFieldNames> result) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::Fields));

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  auto fields = calendar.fields();
  if (!fields) {
    bool arrayIterationSane;
    if (calendar.receiver().isString()) {
      // "String" calendars don't perform observable array iteration.
      arrayIterationSane = true;
    } else {
      // "Object" calendars need to ensure array iteration is still sane.
      if (!IsArrayIterationSane(cx, &arrayIterationSane)) {
        return false;
      }
    }

    if (arrayIterationSane) {
      auto calendarId = BuiltinCalendarId(calendar.receiver());

      // Steps 2.a-b.
      return BuiltinCalendarFields(cx, calendarId, fieldNames, result.get());

      // Steps 2.c-e. (Not applicable in our implementation.)
    }
  }

  // Step 3. (Inlined call to CalendarMethodsRecordCall.)

  auto* array = NewDenseFullyAllocatedArray(cx, fieldNames.size());
  if (!array) {
    return false;
  }
  array->setDenseInitializedLength(fieldNames.size());

  size_t index = 0;
  for (auto calendarField : SortedCalendarFields{fieldNames}) {
    auto* name = ToPropertyName(cx, ::ToTemporalField(calendarField));
    array->initDenseElement(index++, StringValue(name));
  }
  MOZ_ASSERT(index == fieldNames.size());

  Rooted<Value> fieldsArray(cx, ObjectValue(*array));
  Rooted<Value> calendarFieldNames(cx);
  if (fields) {
    Rooted<Value> fieldsFn(cx, ObjectValue(*fields));
    auto thisv = calendar.receiver().toObjectValue();
    if (!Call(cx, fieldsFn, thisv, fieldsArray, &calendarFieldNames)) {
      return false;
    }
  } else {
    auto calendarId = BuiltinCalendarId(calendar.receiver());

    if (!BuiltinCalendarFields(cx, calendarId, fieldsArray,
                               &calendarFieldNames)) {
      return false;
    }
  }

  // Steps 4-5.
  if (!IterableToListOfStrings(cx, calendarFieldNames, result)) {
    return false;
  }

  // The spec sorts the field names in PrepareTemporalFields. Sorting is only
  // needed for user-defined calendars, so our implementation performs this step
  // here instead of in PrepareTemporalFields.
  return SortTemporalFieldNames(cx, result.get());
}

static bool RequireIntegralNumber(JSContext* cx, Handle<Value> value,
                                  Handle<PropertyName*> name,
                                  MutableHandle<Value> result) {
  if (MOZ_LIKELY(value.isInt32())) {
    result.set(value);
    return true;
  }

  if (value.isDouble()) {
    double d = value.toDouble();
    if (js::IsInteger(d)) {
      result.setNumber(d);
      return true;
    }

    if (auto str = QuoteString(cx, name)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INVALID_INTEGER, str.get());
    }
    return false;
  }

  if (auto str = QuoteString(cx, name)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_UNEXPECTED_TYPE, str.get(), "not a number");
  }
  return false;
}

static bool RequireIntegralPositiveNumber(JSContext* cx, Handle<Value> value,
                                          Handle<PropertyName*> name,
                                          MutableHandle<Value> result) {
  if (!RequireIntegralNumber(cx, value, name, result)) {
    return false;
  }

  if (result.toNumber() <= 0) {
    if (auto str = QuoteString(cx, name)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_INVALID_NUMBER, str.get());
    }
    return false;
  }
  return true;
}

static bool RequireIntegralNumberOrUndefined(JSContext* cx, Handle<Value> value,
                                             Handle<PropertyName*> name,
                                             MutableHandle<Value> result) {
  if (value.isUndefined()) {
    result.setUndefined();
    return true;
  }
  return RequireIntegralNumber(cx, value, name, result);
}

static bool RequireIntegralPositiveNumberOrUndefined(
    JSContext* cx, Handle<Value> value, Handle<PropertyName*> name,
    MutableHandle<Value> result) {
  if (value.isUndefined()) {
    result.setUndefined();
    return true;
  }
  return RequireIntegralPositiveNumber(cx, value, name, result);
}

static bool RequireString(JSContext* cx, Handle<Value> value,
                          Handle<PropertyName*> name,
                          MutableHandle<Value> result) {
  if (MOZ_LIKELY(value.isString())) {
    result.set(value);
    return true;
  }

  if (auto str = QuoteString(cx, name)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_UNEXPECTED_TYPE, str.get(), "not a string");
  }
  return false;
}

static bool RequireStringOrUndefined(JSContext* cx, Handle<Value> value,
                                     Handle<PropertyName*> name,
                                     MutableHandle<Value> result) {
  if (value.isUndefined()) {
    result.setUndefined();
    return true;
  }
  return RequireString(cx, value, name, result);
}

static bool RequireBoolean(JSContext* cx, Handle<Value> value,
                           Handle<PropertyName*> name,
                           MutableHandle<Value> result) {
  if (MOZ_LIKELY(value.isBoolean())) {
    result.set(value);
    return true;
  }

  if (auto str = QuoteString(cx, name)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_UNEXPECTED_TYPE, str.get(),
                              "not a boolean");
  }
  return false;
}

using BuiltinCalendarMethod = bool (*)(JSContext* cx, CalendarId,
                                       const PlainDate&, MutableHandle<Value>);

using CalendarConversion = bool (*)(JSContext*, Handle<Value>,
                                    Handle<PropertyName*>,
                                    MutableHandle<Value>);

template <BuiltinCalendarMethod builtin, CalendarConversion conversion>
static bool CallCalendarMethod(JSContext* cx, Handle<PropertyName*> name,
                               JSNative native, Handle<CalendarValue> calendar,
                               Handle<JSObject*> dateLike,
                               const PlainDate& date,
                               MutableHandle<Value> result) {
  // Step 1.
  if (calendar.isString()) {
    auto calendarId = BuiltinCalendarId(calendar);
    return builtin(cx, calendarId, date, result);
  }

  // Step 2.
  Rooted<JSObject*> calendarObj(cx, calendar.toObject());
  JSObject* fn = GetMethod(cx, calendarObj, name);
  if (!fn) {
    return false;
  }

  // Fast-path for the default implementation.
  if (calendarObj->is<CalendarObject>() && IsNativeFunction(fn, native)) {
    auto calendarId = BuiltinCalendarId(calendar);
    return builtin(cx, calendarId, date, result);
  }

  Rooted<JS::Value> fnVal(cx, ObjectValue(*fn));
  Rooted<JS::Value> dateLikeValue(cx, ObjectValue(*dateLike));
  if (!Call(cx, fnVal, calendarObj, dateLikeValue, result)) {
    return false;
  }

  // Steps 3-5.
  return conversion(cx, result, name, result);
}

/**
 * Temporal.Calendar.prototype.era ( temporalDateLike )
 */
static bool BuiltinCalendarEra(JSContext* cx, CalendarId calendarId,
                               const PlainDate& date,
                               MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-5.
  result.setUndefined();
  return true;
}

static bool Calendar_era(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarEra ( calendar, dateLike )
 */
static bool CalendarEra(JSContext* cx, Handle<CalendarValue> calendar,
                        Handle<JSObject*> dateLike, const PlainDate& date,
                        MutableHandle<Value> result) {
  // Steps 1-5.
  return CallCalendarMethod<BuiltinCalendarEra, RequireStringOrUndefined>(
      cx, cx->names().era, Calendar_era, calendar, dateLike, date, result);
}

/**
 * CalendarEra ( calendar, dateLike )
 */
bool js::temporal::CalendarEra(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<PlainDateObject*> dateLike,
                               MutableHandle<Value> result) {
  return CalendarEra(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarEra ( calendar, dateLike )
 */
bool js::temporal::CalendarEra(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<PlainDateTimeObject*> dateLike,
                               MutableHandle<Value> result) {
  return CalendarEra(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarEra ( calendar, dateLike )
 */
bool js::temporal::CalendarEra(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<PlainYearMonthObject*> dateLike,
                               MutableHandle<Value> result) {
  return CalendarEra(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarEra ( calendar, dateLike )
 */
bool js::temporal::CalendarEra(JSContext* cx, Handle<CalendarValue> calendar,
                               const PlainDateTime& dateTime,
                               MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarEra(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.eraYear ( temporalDateLike )
 */
static bool BuiltinCalendarEraYear(JSContext* cx, CalendarId calendarId,
                                   const PlainDate& date,
                                   MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-7.
  result.setUndefined();
  return true;
}

static bool Calendar_eraYear(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarEraYear ( calendar, dateLike )
 */
static bool CalendarEraYear(JSContext* cx, Handle<CalendarValue> calendar,
                            Handle<JSObject*> dateLike, const PlainDate& date,
                            MutableHandle<Value> result) {
  // Steps 1-6.
  return CallCalendarMethod<BuiltinCalendarEraYear,
                            RequireIntegralNumberOrUndefined>(
      cx, cx->names().eraYear, Calendar_eraYear, calendar, dateLike, date,
      result);
}

/**
 * CalendarEraYear ( calendar, dateLike )
 */
bool js::temporal::CalendarEraYear(JSContext* cx,
                                   Handle<CalendarValue> calendar,
                                   Handle<PlainDateObject*> dateLike,
                                   MutableHandle<Value> result) {
  return CalendarEraYear(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarEraYear ( calendar, dateLike )
 */
bool js::temporal::CalendarEraYear(JSContext* cx,
                                   Handle<CalendarValue> calendar,
                                   Handle<PlainDateTimeObject*> dateLike,
                                   MutableHandle<Value> result) {
  return CalendarEraYear(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarEraYear ( calendar, dateLike )
 */
bool js::temporal::CalendarEraYear(JSContext* cx,
                                   Handle<CalendarValue> calendar,
                                   Handle<PlainYearMonthObject*> dateLike,
                                   MutableHandle<Value> result) {
  return CalendarEraYear(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarEraYear ( calendar, dateLike )
 */
bool js::temporal::CalendarEraYear(JSContext* cx,
                                   Handle<CalendarValue> calendar,
                                   const PlainDateTime& dateTime,
                                   MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarEraYear(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.year ( temporalDateLike )
 */
static bool BuiltinCalendarYear(JSContext* cx, CalendarId calendarId,
                                const PlainDate& date,
                                MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setInt32(date.year);
  return true;
}

static bool Calendar_year(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarYear ( calendar, dateLike )
 */
static bool CalendarYear(JSContext* cx, Handle<CalendarValue> calendar,
                         Handle<JSObject*> dateLike, const PlainDate& date,
                         MutableHandle<Value> result) {
  // Steps 1-5.
  return CallCalendarMethod<BuiltinCalendarYear, RequireIntegralNumber>(
      cx, cx->names().year, Calendar_year, calendar, dateLike, date, result);
}

/**
 * CalendarYear ( calendar, dateLike )
 */
bool js::temporal::CalendarYear(JSContext* cx, Handle<CalendarValue> calendar,
                                Handle<PlainDateObject*> dateLike,
                                MutableHandle<Value> result) {
  return CalendarYear(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarYear ( calendar, dateLike )
 */
bool js::temporal::CalendarYear(JSContext* cx, Handle<CalendarValue> calendar,
                                Handle<PlainDateTimeObject*> dateLike,
                                MutableHandle<Value> result) {
  return CalendarYear(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarYear ( calendar, dateLike )
 */
bool js::temporal::CalendarYear(JSContext* cx, Handle<CalendarValue> calendar,
                                Handle<PlainYearMonthObject*> dateLike,
                                MutableHandle<Value> result) {
  return CalendarYear(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarYear ( calendar, dateLike )
 */
bool js::temporal::CalendarYear(JSContext* cx, Handle<CalendarValue> calendar,
                                const PlainDateTime& dateTime,
                                MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarYear(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.month ( temporalDateLike )
 */
static bool BuiltinCalendarMonth(JSContext* cx, CalendarId calendarId,
                                 const PlainDate& date,
                                 MutableHandle<Value> result) {
  // Steps 1-4. (Not applicable.)

  // Steps 5-7.
  result.setInt32(date.month);
  return true;
}

static bool Calendar_month(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarMonth ( calendar, dateLike )
 */
static bool CalendarMonth(JSContext* cx, Handle<CalendarValue> calendar,
                          Handle<JSObject*> dateLike, const PlainDate& date,
                          MutableHandle<Value> result) {
  // Steps 1-6.
  return CallCalendarMethod<BuiltinCalendarMonth,
                            RequireIntegralPositiveNumber>(
      cx, cx->names().month, Calendar_month, calendar, dateLike, date, result);
}

/**
 * CalendarMonth ( calendar, dateLike )
 */
bool js::temporal::CalendarMonth(JSContext* cx, Handle<CalendarValue> calendar,
                                 Handle<PlainDateObject*> dateLike,
                                 MutableHandle<Value> result) {
  return CalendarMonth(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarMonth ( calendar, dateLike )
 */
bool js::temporal::CalendarMonth(JSContext* cx, Handle<CalendarValue> calendar,
                                 Handle<PlainDateTimeObject*> dateLike,
                                 MutableHandle<Value> result) {
  return CalendarMonth(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarMonth ( calendar, dateLike )
 */
bool js::temporal::CalendarMonth(JSContext* cx, Handle<CalendarValue> calendar,
                                 Handle<PlainYearMonthObject*> dateLike,
                                 MutableHandle<Value> result) {
  return CalendarMonth(cx, calendar, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarMonth ( calendar, dateLike )
 */
bool js::temporal::CalendarMonth(JSContext* cx, Handle<CalendarValue> calendar,
                                 const PlainDateTime& dateTime,
                                 MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarMonth(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.monthCode ( temporalDateLike )
 */
static bool BuiltinCalendarMonthCode(JSContext* cx, CalendarId calendarId,
                                     const PlainDate& date,
                                     MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  JSString* str = ISOMonthCode(cx, date.month);
  if (!str) {
    return false;
  }

  result.setString(str);
  return true;
}

static bool Calendar_monthCode(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
static bool CalendarMonthCode(JSContext* cx, Handle<CalendarValue> calendar,
                              Handle<JSObject*> dateLike, const PlainDate& date,
                              MutableHandle<Value> result) {
  // Steps 1-4.
  return CallCalendarMethod<BuiltinCalendarMonthCode, RequireString>(
      cx, cx->names().monthCode, Calendar_monthCode, calendar, dateLike, date,
      result);
}

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
bool js::temporal::CalendarMonthCode(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     Handle<PlainDateObject*> dateLike,
                                     MutableHandle<Value> result) {
  return CalendarMonthCode(cx, calendar, dateLike, ToPlainDate(dateLike),
                           result);
}

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
bool js::temporal::CalendarMonthCode(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     Handle<PlainDateTimeObject*> dateLike,
                                     MutableHandle<Value> result) {
  return CalendarMonthCode(cx, calendar, dateLike, ToPlainDate(dateLike),
                           result);
}

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
bool js::temporal::CalendarMonthCode(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     Handle<PlainMonthDayObject*> dateLike,
                                     MutableHandle<Value> result) {
  return CalendarMonthCode(cx, calendar, dateLike, ToPlainDate(dateLike),
                           result);
}

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
bool js::temporal::CalendarMonthCode(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     Handle<PlainYearMonthObject*> dateLike,
                                     MutableHandle<Value> result) {
  return CalendarMonthCode(cx, calendar, dateLike, ToPlainDate(dateLike),
                           result);
}

/**
 * CalendarMonthCode ( calendar, dateLike )
 */
bool js::temporal::CalendarMonthCode(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     const PlainDateTime& dateTime,
                                     MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarMonthCode(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.day ( temporalDateLike )
 */
static bool BuiltinCalendarDay(JSContext* cx, CalendarId calendarId,
                               const PlainDate& date,
                               MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setInt32(date.day);
  return true;
}

/**
 * CalendarDay ( calendarRec, dateLike )
 */
static bool CalendarDay(JSContext* cx, Handle<CalendarRecord> calendar,
                        Handle<JSObject*> dateLike, const PlainDate& date,
                        MutableHandle<Value> result) {
  MOZ_ASSERT(CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::Day));

  // Step 2. (Reordered)
  auto day = calendar.day();
  if (!day) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());
    return BuiltinCalendarDay(cx, calendarId, date, result);
  }

  // Step 1. (Inlined call to CalendarMethodsRecordCall.)
  Rooted<Value> fn(cx, ObjectValue(*day));
  auto thisv = calendar.receiver().toObjectValue();
  Rooted<JS::Value> dateLikeValue(cx, ObjectValue(*dateLike));
  if (!Call(cx, fn, thisv, dateLikeValue, result)) {
    return false;
  }

  // Steps 3-6.
  return RequireIntegralPositiveNumber(cx, result, cx->names().day, result);
}

/**
 * CalendarDay ( calendarRec, dateLike )
 */
bool js::temporal::CalendarDay(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<PlainDateObject*> dateLike,
                               MutableHandle<Value> result) {
  Rooted<CalendarRecord> calendarRec(cx);
  if (!CreateCalendarMethodsRecord(cx, calendar,
                                   {
                                       CalendarMethod::Day,
                                   },
                                   &calendarRec)) {
    return false;
  }

  return CalendarDay(cx, calendarRec, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarDay ( calendarRec, dateLike )
 */
bool js::temporal::CalendarDay(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<PlainDateTimeObject*> dateLike,
                               MutableHandle<Value> result) {
  Rooted<CalendarRecord> calendarRec(cx);
  if (!CreateCalendarMethodsRecord(cx, calendar,
                                   {
                                       CalendarMethod::Day,
                                   },
                                   &calendarRec)) {
    return false;
  }

  return CalendarDay(cx, calendarRec, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarDay ( calendarRec, dateLike )
 */
bool js::temporal::CalendarDay(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<PlainMonthDayObject*> dateLike,
                               MutableHandle<Value> result) {
  Rooted<CalendarRecord> calendarRec(cx);
  if (!CreateCalendarMethodsRecord(cx, calendar,
                                   {
                                       CalendarMethod::Day,
                                   },
                                   &calendarRec)) {
    return false;
  }

  return CalendarDay(cx, calendarRec, dateLike, ToPlainDate(dateLike), result);
}

/**
 * CalendarDay ( calendarRec, dateLike )
 */
bool js::temporal::CalendarDay(JSContext* cx, Handle<CalendarRecord> calendar,
                               const PlainDate& date,
                               MutableHandle<Value> result) {
  Rooted<PlainDateObject*> dateLike(
      cx, CreateTemporalDate(cx, date, calendar.receiver()));
  if (!dateLike) {
    return false;
  }

  return ::CalendarDay(cx, calendar, dateLike, date, result);
}

/**
 * CalendarDay ( calendarRec, dateLike )
 */
bool js::temporal::CalendarDay(JSContext* cx, Handle<CalendarRecord> calendar,
                               const PlainDateTime& dateTime,
                               MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar.receiver()));
  if (!dateLike) {
    return false;
  }

  return ::CalendarDay(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.dayOfWeek ( temporalDateLike )
 */
static bool BuiltinCalendarDayOfWeek(JSContext* cx, CalendarId calendarId,
                                     const PlainDate& date,
                                     MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setInt32(ToISODayOfWeek(date));
  return true;
}

static bool Calendar_dayOfWeek(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarDayOfWeek ( calendar, dateLike )
 */
static bool CalendarDayOfWeek(JSContext* cx, Handle<CalendarValue> calendar,
                              Handle<JSObject*> dateLike, const PlainDate& date,
                              MutableHandle<Value> result) {
  // Steps 1-6.
  return CallCalendarMethod<BuiltinCalendarDayOfWeek,
                            RequireIntegralPositiveNumber>(
      cx, cx->names().dayOfWeek, Calendar_dayOfWeek, calendar, dateLike, date,
      result);
}

/**
 * CalendarDayOfWeek ( calendar, dateLike )
 */
bool js::temporal::CalendarDayOfWeek(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     Handle<PlainDateObject*> dateLike,
                                     MutableHandle<Value> result) {
  return CalendarDayOfWeek(cx, calendar, dateLike, ToPlainDate(dateLike),
                           result);
}

/**
 * CalendarDayOfWeek ( calendar, dateLike )
 */
bool js::temporal::CalendarDayOfWeek(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     Handle<PlainDateTimeObject*> dateLike,
                                     MutableHandle<Value> result) {
  return CalendarDayOfWeek(cx, calendar, dateLike, ToPlainDate(dateLike),
                           result);
}

/**
 * CalendarDayOfWeek ( calendar, dateLike )
 */
bool js::temporal::CalendarDayOfWeek(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     const PlainDateTime& dateTime,
                                     MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarDayOfWeek(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.dayOfYear ( temporalDateLike )
 */
static bool BuiltinCalendarDayOfYear(JSContext* cx, CalendarId calendarId,
                                     const PlainDate& date,
                                     MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setInt32(ToISODayOfYear(date));
  return true;
}

static bool Calendar_dayOfYear(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarDayOfYear ( calendar, dateLike )
 */
static bool CalendarDayOfYear(JSContext* cx, Handle<CalendarValue> calendar,
                              Handle<JSObject*> dateLike, const PlainDate& date,
                              MutableHandle<Value> result) {
  // Steps 1-6.
  return CallCalendarMethod<BuiltinCalendarDayOfYear,
                            RequireIntegralPositiveNumber>(
      cx, cx->names().dayOfYear, Calendar_dayOfYear, calendar, dateLike, date,
      result);
}

/**
 * CalendarDayOfYear ( calendar, dateLike )
 */
bool js::temporal::CalendarDayOfYear(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     Handle<PlainDateObject*> dateLike,
                                     MutableHandle<Value> result) {
  return CalendarDayOfYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                           result);
}

/**
 * CalendarDayOfYear ( calendar, dateLike )
 */
bool js::temporal::CalendarDayOfYear(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     Handle<PlainDateTimeObject*> dateLike,
                                     MutableHandle<Value> result) {
  return CalendarDayOfYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                           result);
}

/**
 * CalendarDayOfYear ( calendar, dateLike )
 */
bool js::temporal::CalendarDayOfYear(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     const PlainDateTime& dateTime,
                                     MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarDayOfYear(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.weekOfYear ( temporalDateLike )
 */
static bool BuiltinCalendarWeekOfYear(JSContext* cx, CalendarId calendarId,
                                      const PlainDate& date,
                                      MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setInt32(ToISOWeekOfYear(date).week);
  return true;
}

static bool Calendar_weekOfYear(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarWeekOfYear ( calendar, dateLike )
 */
static bool CalendarWeekOfYear(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<JSObject*> dateLike,
                               const PlainDate& date,
                               MutableHandle<Value> result) {
  // Steps 1-6.
  return CallCalendarMethod<BuiltinCalendarWeekOfYear,
                            RequireIntegralPositiveNumberOrUndefined>(
      cx, cx->names().weekOfYear, Calendar_weekOfYear, calendar, dateLike, date,
      result);
}

/**
 * CalendarWeekOfYear ( calendar, dateLike )
 */
bool js::temporal::CalendarWeekOfYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainDateObject*> dateLike,
                                      MutableHandle<Value> result) {
  return CalendarWeekOfYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                            result);
}

/**
 * CalendarWeekOfYear ( calendar, dateLike )
 */
bool js::temporal::CalendarWeekOfYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainDateTimeObject*> dateLike,
                                      MutableHandle<Value> result) {
  return CalendarWeekOfYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                            result);
}

/**
 * CalendarWeekOfYear ( calendar, dateLike )
 */
bool js::temporal::CalendarWeekOfYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      const PlainDateTime& dateTime,
                                      MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarWeekOfYear(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.yearOfWeek ( temporalDateLike )
 */
static bool BuiltinCalendarYearOfWeek(JSContext* cx, CalendarId calendarId,
                                      const PlainDate& date,
                                      MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setInt32(ToISOWeekOfYear(date).year);
  return true;
}

static bool Calendar_yearOfWeek(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarYearOfWeek ( calendar, dateLike )
 */
static bool CalendarYearOfWeek(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<JSObject*> dateLike,
                               const PlainDate& date,
                               MutableHandle<Value> result) {
  // Steps 1-5.
  return CallCalendarMethod<BuiltinCalendarYearOfWeek,
                            RequireIntegralNumberOrUndefined>(
      cx, cx->names().yearOfWeek, Calendar_yearOfWeek, calendar, dateLike, date,
      result);
}

/**
 * CalendarYearOfWeek ( calendar, dateLike )
 */
bool js::temporal::CalendarYearOfWeek(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainDateObject*> dateLike,
                                      MutableHandle<Value> result) {
  return CalendarYearOfWeek(cx, calendar, dateLike, ToPlainDate(dateLike),
                            result);
}

/**
 * CalendarYearOfWeek ( calendar, dateLike )
 */
bool js::temporal::CalendarYearOfWeek(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainDateTimeObject*> dateLike,
                                      MutableHandle<Value> result) {
  return CalendarYearOfWeek(cx, calendar, dateLike, ToPlainDate(dateLike),
                            result);
}

/**
 * CalendarYearOfWeek ( calendar, dateLike )
 */
bool js::temporal::CalendarYearOfWeek(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      const PlainDateTime& dateTime,
                                      MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarYearOfWeek(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.daysInWeek ( temporalDateLike )
 */
static bool BuiltinCalendarDaysInWeek(JSContext* cx, CalendarId calendarId,
                                      const PlainDate& date,
                                      MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setInt32(7);
  return true;
}

static bool Calendar_daysInWeek(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarDaysInWeek ( calendar, dateLike )
 */
static bool CalendarDaysInWeek(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<JSObject*> dateLike,
                               const PlainDate& date,
                               MutableHandle<Value> result) {
  // Steps 1-6.
  return CallCalendarMethod<BuiltinCalendarDaysInWeek,
                            RequireIntegralPositiveNumber>(
      cx, cx->names().daysInWeek, Calendar_daysInWeek, calendar, dateLike, date,
      result);
}

/**
 * CalendarDaysInWeek ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInWeek(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainDateObject*> dateLike,
                                      MutableHandle<Value> result) {
  return CalendarDaysInWeek(cx, calendar, dateLike, ToPlainDate(dateLike),
                            result);
}

/**
 * CalendarDaysInWeek ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInWeek(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainDateTimeObject*> dateLike,
                                      MutableHandle<Value> result) {
  return CalendarDaysInWeek(cx, calendar, dateLike, ToPlainDate(dateLike),
                            result);
}

/**
 * CalendarDaysInWeek ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInWeek(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      const PlainDateTime& dateTime,
                                      MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarDaysInWeek(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.daysInMonth ( temporalDateLike )
 */
static bool BuiltinCalendarDaysInMonth(JSContext* cx, CalendarId calendarId,
                                       const PlainDate& date,
                                       MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setInt32(::ISODaysInMonth(date.year, date.month));
  return true;
}

static bool Calendar_daysInMonth(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarDaysInMonth ( calendar, dateLike )
 */
static bool CalendarDaysInMonth(JSContext* cx, Handle<CalendarValue> calendar,
                                Handle<JSObject*> dateLike,
                                const PlainDate& date,
                                MutableHandle<Value> result) {
  // Step 1-6.
  return CallCalendarMethod<BuiltinCalendarDaysInMonth,
                            RequireIntegralPositiveNumber>(
      cx, cx->names().daysInMonth, Calendar_daysInMonth, calendar, dateLike,
      date, result);
}

/**
 * CalendarDaysInMonth ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInMonth(JSContext* cx,
                                       Handle<CalendarValue> calendar,
                                       Handle<PlainDateObject*> dateLike,
                                       MutableHandle<Value> result) {
  return CalendarDaysInMonth(cx, calendar, dateLike, ToPlainDate(dateLike),
                             result);
}

/**
 * CalendarDaysInMonth ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInMonth(JSContext* cx,
                                       Handle<CalendarValue> calendar,
                                       Handle<PlainDateTimeObject*> dateLike,
                                       MutableHandle<Value> result) {
  return CalendarDaysInMonth(cx, calendar, dateLike, ToPlainDate(dateLike),
                             result);
}

/**
 * CalendarDaysInMonth ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInMonth(JSContext* cx,
                                       Handle<CalendarValue> calendar,
                                       Handle<PlainYearMonthObject*> dateLike,
                                       MutableHandle<Value> result) {
  return CalendarDaysInMonth(cx, calendar, dateLike, ToPlainDate(dateLike),
                             result);
}

/**
 * CalendarDaysInMonth ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInMonth(JSContext* cx,
                                       Handle<CalendarValue> calendar,
                                       const PlainDateTime& dateTime,
                                       MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarDaysInMonth(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.daysInYear ( temporalDateLike )
 */
static bool BuiltinCalendarDaysInYear(JSContext* cx, CalendarId calendarId,
                                      const PlainDate& date,
                                      MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setInt32(ISODaysInYear(date.year));
  return true;
}

static bool Calendar_daysInYear(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarDaysInYear ( calendar, dateLike )
 */
static bool CalendarDaysInYear(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<JSObject*> dateLike,
                               const PlainDate& date,
                               MutableHandle<Value> result) {
  // Step 1-6.
  return CallCalendarMethod<BuiltinCalendarDaysInYear,
                            RequireIntegralPositiveNumber>(
      cx, cx->names().daysInYear, Calendar_daysInYear, calendar, dateLike, date,
      result);
}

/**
 * CalendarDaysInYear ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainDateObject*> dateLike,
                                      MutableHandle<Value> result) {
  return CalendarDaysInYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                            result);
}

/**
 * CalendarDaysInYear ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainDateTimeObject*> dateLike,
                                      MutableHandle<Value> result) {
  return CalendarDaysInYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                            result);
}

/**
 * CalendarDaysInYear ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainYearMonthObject*> dateLike,
                                      MutableHandle<Value> result) {
  return CalendarDaysInYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                            result);
}

/**
 * CalendarDaysInYear ( calendar, dateLike )
 */
bool js::temporal::CalendarDaysInYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      const PlainDateTime& dateTime,
                                      MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarDaysInYear(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.monthsInYear ( temporalDateLike )
 */
static bool BuiltinCalendarMonthsInYear(JSContext* cx, CalendarId calendarId,
                                        const PlainDate& date,
                                        MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setInt32(12);
  return true;
}

static bool Calendar_monthsInYear(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarMonthsInYear ( calendar, dateLike )
 */
static bool CalendarMonthsInYear(JSContext* cx, Handle<CalendarValue> calendar,
                                 Handle<JSObject*> dateLike,
                                 const PlainDate& date,
                                 MutableHandle<Value> result) {
  // Step 1-6.
  return CallCalendarMethod<BuiltinCalendarMonthsInYear,
                            RequireIntegralPositiveNumber>(
      cx, cx->names().monthsInYear, Calendar_monthsInYear, calendar, dateLike,
      date, result);
}

/**
 * CalendarMonthsInYear ( calendar, dateLike )
 */
bool js::temporal::CalendarMonthsInYear(JSContext* cx,
                                        Handle<CalendarValue> calendar,
                                        Handle<PlainDateObject*> dateLike,
                                        MutableHandle<Value> result) {
  return ::CalendarMonthsInYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                                result);
}

/**
 * CalendarMonthsInYear ( calendar, dateLike )
 */
bool js::temporal::CalendarMonthsInYear(JSContext* cx,
                                        Handle<CalendarValue> calendar,
                                        Handle<PlainDateTimeObject*> dateLike,
                                        MutableHandle<Value> result) {
  return ::CalendarMonthsInYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                                result);
}

/**
 * CalendarMonthsInYear ( calendar, dateLike )
 */
bool js::temporal::CalendarMonthsInYear(JSContext* cx,
                                        Handle<CalendarValue> calendar,
                                        Handle<PlainYearMonthObject*> dateLike,
                                        MutableHandle<Value> result) {
  return ::CalendarMonthsInYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                                result);
}

/**
 * CalendarMonthsInYear ( calendar, dateLike )
 */
bool js::temporal::CalendarMonthsInYear(JSContext* cx,
                                        Handle<CalendarValue> calendar,
                                        const PlainDateTime& dateTime,
                                        MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarMonthsInYear(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * Temporal.Calendar.prototype.inLeapYear ( temporalDateLike )
 */
static bool BuiltinCalendarInLeapYear(JSContext* cx, CalendarId calendarId,
                                      const PlainDate& date,
                                      MutableHandle<Value> result) {
  // Steps 1-3. (Not applicable.)

  // Steps 4-6.
  result.setBoolean(IsISOLeapYear(date.year));
  return true;
}

static bool Calendar_inLeapYear(JSContext* cx, unsigned argc, Value* vp);

/**
 * CalendarInLeapYear ( calendar, dateLike )
 */
static bool CalendarInLeapYear(JSContext* cx, Handle<CalendarValue> calendar,
                               Handle<JSObject*> dateLike,
                               const PlainDate& date,
                               MutableHandle<Value> result) {
  // Step 1-4.
  return CallCalendarMethod<BuiltinCalendarInLeapYear, RequireBoolean>(
      cx, cx->names().inLeapYear, Calendar_inLeapYear, calendar, dateLike, date,
      result);
}

/**
 * CalendarInLeapYear ( calendar, dateLike )
 */
bool js::temporal::CalendarInLeapYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainDateObject*> dateLike,
                                      MutableHandle<Value> result) {
  return ::CalendarInLeapYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                              result);
}

/**
 * CalendarInLeapYear ( calendar, dateLike )
 */
bool js::temporal::CalendarInLeapYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainDateTimeObject*> dateLike,
                                      MutableHandle<Value> result) {
  return ::CalendarInLeapYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                              result);
}

/**
 * CalendarInLeapYear ( calendar, dateLike )
 */
bool js::temporal::CalendarInLeapYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      Handle<PlainYearMonthObject*> dateLike,
                                      MutableHandle<Value> result) {
  return ::CalendarInLeapYear(cx, calendar, dateLike, ToPlainDate(dateLike),
                              result);
}

/**
 * CalendarInLeapYear ( calendar, dateLike )
 */
bool js::temporal::CalendarInLeapYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      const PlainDateTime& dateTime,
                                      MutableHandle<Value> result) {
  Rooted<PlainDateTimeObject*> dateLike(
      cx, CreateTemporalDateTime(cx, dateTime, calendar));
  if (!dateLike) {
    return false;
  }

  return ::CalendarInLeapYear(cx, calendar, dateLike, dateTime.date, result);
}

/**
 * ISOResolveMonth ( fields )
 */
static bool ISOResolveMonth(JSContext* cx,
                            MutableHandle<TemporalFields> fields) {
  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  double month = fields.month();

  // Step 3.
  MOZ_ASSERT((IsInteger(month) && month > 0) || std::isnan(month));

  // Step 4.
  Handle<JSString*> monthCode = fields.monthCode();

  // Step 5.
  if (!monthCode) {
    // Step 5.a.
    if (std::isnan(month)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_CALENDAR_MISSING_FIELD,
                                "monthCode");
      return false;
    }

    // Step 5.b.
    return true;
  }

  // Steps 6-7. (Not applicable in our implementation.)

  // Step 8.
  if (monthCode->length() != 3) {
    if (auto code = QuoteString(cx, monthCode)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_CALENDAR_INVALID_MONTHCODE,
                               code.get());
    }
    return false;
  }

  JSLinearString* linear = monthCode->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  char16_t chars[3] = {
      linear->latin1OrTwoByteChar(0),
      linear->latin1OrTwoByteChar(1),
      linear->latin1OrTwoByteChar(2),
  };

  // Steps 9-11. (Partial)
  if (chars[0] != 'M' || !mozilla::IsAsciiDigit(chars[1]) ||
      !mozilla::IsAsciiDigit(chars[2])) {
    if (auto code = QuoteString(cx, linear)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_CALENDAR_INVALID_MONTHCODE,
                               code.get());
    }
    return false;
  }

  // Step 12.
  int32_t monthCodeInteger =
      AsciiDigitToNumber(chars[1]) * 10 + AsciiDigitToNumber(chars[2]);

  // Step 11. (Partial)
  if (monthCodeInteger < 1 || monthCodeInteger > 12) {
    if (auto code = QuoteString(cx, linear)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_CALENDAR_INVALID_MONTHCODE,
                               code.get());
    }
    return false;
  }

  // Step 13. (Not applicable in our implementation.)

  // Step 14.
  if (!std::isnan(month) && month != monthCodeInteger) {
    if (auto code = QuoteString(cx, linear)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_CALENDAR_INVALID_MONTHCODE,
                               code.get());
    }
    return false;
  }

  // Step 15.
  fields.month() = monthCodeInteger;

  // Step 16.
  return true;
}

/**
 * ISODateFromFields ( fields, overflow )
 */
static bool ISODateFromFields(JSContext* cx, Handle<TemporalFields> fields,
                              TemporalOverflow overflow, PlainDate* result) {
  // Steps 1-2. (Not applicable in our implementation.)

  // Step 3.
  double year = fields.year();

  // Step 4.
  double month = fields.month();

  // Step 5.
  double day = fields.day();

  // Step 6.
  MOZ_ASSERT(!std::isnan(year) && !std::isnan(month) && !std::isnan(day));

  // Step 7.
  RegulatedISODate regulated;
  if (!RegulateISODate(cx, year, month, day, overflow, &regulated)) {
    return false;
  }

  // The result is used to create a new PlainDateObject, so it's okay to
  // directly throw an error for invalid years. That way we don't have to worry
  // about representing doubles in PlainDate structs.
  int32_t intYear;
  if (!mozilla::NumberEqualsInt32(regulated.year, &intYear)) {
    // CreateTemporalDate, steps 1-2.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  *result = {intYear, regulated.month, regulated.day};
  return true;
}

/**
 * Temporal.Calendar.prototype.dateFromFields ( fields [ , options ] )
 */
static PlainDateObject* BuiltinCalendarDateFromFields(
    JSContext* cx, CalendarId calendarId, Handle<JSObject*> fields,
    Handle<JSObject*> maybeOptions) {
  // Steps 1-4. (Not applicable)

  // Step 5.
  auto relevantFieldNames = {TemporalField::Day, TemporalField::Month,
                             TemporalField::MonthCode, TemporalField::Year};

  // Steps 6-7.
  Rooted<TemporalFields> dateFields(cx);
  if (!PrepareTemporalFields(cx, fields, relevantFieldNames,
                             {TemporalField::Day, TemporalField::Year},
                             &dateFields)) {
    return nullptr;
  }

  // Step 8.
  auto overflow = TemporalOverflow::Constrain;
  if (maybeOptions) {
    if (!GetTemporalOverflowOption(cx, maybeOptions, &overflow)) {
      return nullptr;
    }
  }

  // Step 9.a.
  if (!ISOResolveMonth(cx, &dateFields)) {
    return nullptr;
  }

  // Step 9.b.
  PlainDate result;
  if (!ISODateFromFields(cx, dateFields, overflow, &result)) {
    return nullptr;
  }

  // Step 11.
  Rooted<CalendarValue> calendar(cx, CalendarValue(calendarId));
  return CreateTemporalDate(cx, result, calendar);
}

/**
 * CalendarDateFromFields ( calendarRec, fields [ , options ] )
 */
static Wrapped<PlainDateObject*> CalendarDateFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar, Handle<JSObject*> fields,
    Handle<PlainObject*> maybeOptions) {
  MOZ_ASSERT(CalendarMethodsRecordHasLookedUp(calendar,
                                              CalendarMethod::DateFromFields));

  // Step 1. (Not applicable in our implemetation.)

  // Step 3. (Reordered)
  auto dateFromFields = calendar.dateFromFields();
  if (!dateFromFields) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());
    return BuiltinCalendarDateFromFields(cx, calendarId, fields, maybeOptions);
  }

  // Step 2. (Inlined call to CalendarMethodsRecordCall.)

  Rooted<Value> dateFromFieldsFn(cx, ObjectValue(*dateFromFields));
  auto thisv = calendar.receiver().toObjectValue();
  Rooted<Value> rval(cx);

  FixedInvokeArgs<2> args(cx);
  args[0].setObject(*fields);
  if (maybeOptions) {
    args[1].setObject(*maybeOptions);
  } else {
    args[1].setUndefined();
  }

  if (!Call(cx, dateFromFieldsFn, thisv, args, &rval)) {
    return nullptr;
  }

  // Step 4.
  if (!rval.isObject() || !rval.toObject().canUnwrapAs<PlainDateObject>()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, rval,
                     nullptr, "not a PlainDate object");
    return nullptr;
  }

  // Step 5.
  return &rval.toObject();
}

/**
 * CalendarDateFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainDateObject*> js::temporal::CalendarDateFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<PlainObject*> fields) {
  // Steps 1-6.
  return ::CalendarDateFromFields(cx, calendar, fields, nullptr);
}

/**
 * CalendarDateFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainDateObject*> js::temporal::CalendarDateFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar, Handle<PlainObject*> fields,
    Handle<PlainObject*> options) {
  // Steps 1-6.
  return ::CalendarDateFromFields(cx, calendar, fields, options);
}

struct RegulatedISOYearMonth final {
  double year = 0;
  int32_t month = 0;
};

/**
 * RegulateISOYearMonth ( year, month, overflow )
 */
static bool RegulateISOYearMonth(JSContext* cx, double year, double month,
                                 TemporalOverflow overflow,
                                 RegulatedISOYearMonth* result) {
  MOZ_ASSERT(IsInteger(year));
  MOZ_ASSERT(IsInteger(month));

  // Step 1.
  if (overflow == TemporalOverflow::Constrain) {
    // Step 1.a.
    month = std::clamp(month, 1.0, 12.0);

    // Step 1.b.
    *result = {year, int32_t(month)};
    return true;
  }

  // Step 2.a.
  MOZ_ASSERT(overflow == TemporalOverflow::Reject);

  // Step 2.b.
  if (month < 1 || month > 12) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_YEAR_MONTH_INVALID);
    return false;
  }

  // Step 2.c.
  *result = {year, int32_t(month)};
  return true;
}

/**
 * ISOYearMonthFromFields ( fields, overflow )
 */
static bool ISOYearMonthFromFields(JSContext* cx, Handle<TemporalFields> fields,
                                   TemporalOverflow overflow,
                                   PlainDate* result) {
  // Step 1.
  double year = fields.year();

  // Step 2.
  double month = fields.month();

  // Step 3.
  MOZ_ASSERT(!std::isnan(year) && !std::isnan(month));

  // Step 4.
  RegulatedISOYearMonth regulated;
  if (!RegulateISOYearMonth(cx, year, month, overflow, &regulated)) {
    return false;
  }

  // Step 5.

  // The result is used to create a new PlainYearMonthObject, so it's okay to
  // directly throw an error for invalid years. That way we don't have to worry
  // about representing doubles in PlainDate structs.
  int32_t intYear;
  if (!mozilla::NumberEqualsInt32(regulated.year, &intYear)) {
    // CreateTemporalYearMonth, steps 1-2.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_YEAR_MONTH_INVALID);
    return false;
  }

  *result = {intYear, regulated.month, 1};
  return true;
}

/**
 * Temporal.Calendar.prototype.yearMonthFromFields ( fields [ , options ] )
 */
static PlainYearMonthObject* BuiltinCalendarYearMonthFromFields(
    JSContext* cx, CalendarId calendarId, Handle<JSObject*> fields,
    Handle<JSObject*> maybeOptions) {
  // Steps 1-4. (Not applicable)

  // Step 5.
  auto relevantFieldNames = {TemporalField::Month, TemporalField::MonthCode,
                             TemporalField::Year};

  // Steps 6-7.
  Rooted<TemporalFields> dateFields(cx);
  if (!PrepareTemporalFields(cx, fields, relevantFieldNames,
                             {TemporalField::Year}, &dateFields)) {
    return nullptr;
  }

  // Step 8.
  auto overflow = TemporalOverflow::Constrain;
  if (maybeOptions) {
    if (!GetTemporalOverflowOption(cx, maybeOptions, &overflow)) {
      return nullptr;
    }
  }

  // Step 9.a.
  if (!ISOResolveMonth(cx, &dateFields)) {
    return nullptr;
  }

  // Step 9.b.
  PlainDate result;
  if (!ISOYearMonthFromFields(cx, dateFields, overflow, &result)) {
    return nullptr;
  }

  // Step 11.
  Rooted<CalendarValue> calendar(cx, CalendarValue(calendarId));
  return CreateTemporalYearMonth(cx, result, calendar);
}

/**
 * CalendarYearMonthFromFields ( calendarRec, fields [ , options ] )
 */
static Wrapped<PlainYearMonthObject*> CalendarYearMonthFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar, Handle<JSObject*> fields,
    Handle<PlainObject*> maybeOptions) {
  MOZ_ASSERT(CalendarMethodsRecordHasLookedUp(
      calendar, CalendarMethod::YearMonthFromFields));

  // Step 1. (Not applicable in our implementation.)

  // Step 3. (Reordered)
  auto yearMonthFromFields = calendar.yearMonthFromFields();
  if (!yearMonthFromFields) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());
    return BuiltinCalendarYearMonthFromFields(cx, calendarId, fields,
                                              maybeOptions);
  }

  // Step 2. (Inlined call to CalendarMethodsRecordCall.)

  Rooted<Value> yearMonthFromFieldsFn(cx, ObjectValue(*yearMonthFromFields));
  auto thisv = calendar.receiver().toObjectValue();
  Rooted<Value> rval(cx);

  FixedInvokeArgs<2> args(cx);
  args[0].setObject(*fields);
  if (maybeOptions) {
    args[1].setObject(*maybeOptions);
  } else {
    args[1].setUndefined();
  }

  if (!Call(cx, yearMonthFromFieldsFn, thisv, args, &rval)) {
    return nullptr;
  }

  // Step 4.
  if (!rval.isObject() ||
      !rval.toObject().canUnwrapAs<PlainYearMonthObject>()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, rval,
                     nullptr, "not a PlainYearMonth object");
    return nullptr;
  }

  // Step 5.
  return &rval.toObject();
}

/**
 * CalendarYearMonthFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainYearMonthObject*> js::temporal::CalendarYearMonthFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<PlainObject*> fields) {
  // Steps 1-4.
  return ::CalendarYearMonthFromFields(cx, calendar, fields, nullptr);
}

/**
 * CalendarYearMonthFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainYearMonthObject*> js::temporal::CalendarYearMonthFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<PlainYearMonthObject*> fields) {
  // Steps 1-4.
  return ::CalendarYearMonthFromFields(cx, calendar, fields, nullptr);
}

/**
 * CalendarYearMonthFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainYearMonthObject*> js::temporal::CalendarYearMonthFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar, Handle<PlainObject*> fields,
    Handle<PlainObject*> options) {
  // Steps 1-4.
  return ::CalendarYearMonthFromFields(cx, calendar, fields, options);
}

/**
 * ISOMonthDayFromFields ( fields, overflow )
 */
static bool ISOMonthDayFromFields(JSContext* cx, Handle<TemporalFields> fields,
                                  TemporalOverflow overflow,
                                  PlainDate* result) {
  // Step 1.
  double month = fields.month();

  // Step 2.
  double day = fields.day();

  // Step 3.
  MOZ_ASSERT(!std::isnan(month));
  MOZ_ASSERT(!std::isnan(day));

  // Step 4.
  double year = fields.year();

  // Step 5.
  int32_t referenceISOYear = 1972;

  // Steps 6-7.
  double y = std::isnan(year) ? referenceISOYear : year;
  RegulatedISODate regulated;
  if (!RegulateISODate(cx, y, month, day, overflow, &regulated)) {
    return false;
  }

  // Step 8.
  *result = {referenceISOYear, regulated.month, regulated.day};
  return true;
}

/**
 * Temporal.Calendar.prototype.monthDayFromFields ( fields [ , options ] )
 */
static PlainMonthDayObject* BuiltinCalendarMonthDayFromFields(
    JSContext* cx, CalendarId calendarId, Handle<JSObject*> fields,
    Handle<JSObject*> maybeOptions) {
  // Steps 1-4. (Not applicable)

  // Step 5.
  auto relevantFieldNames = {TemporalField::Day, TemporalField::Month,
                             TemporalField::MonthCode, TemporalField::Year};

  // Steps 6-7.
  Rooted<TemporalFields> dateFields(cx);
  if (!PrepareTemporalFields(cx, fields, relevantFieldNames,
                             {TemporalField::Day}, &dateFields)) {
    return nullptr;
  }

  // Step 8.
  auto overflow = TemporalOverflow::Constrain;
  if (maybeOptions) {
    if (!GetTemporalOverflowOption(cx, maybeOptions, &overflow)) {
      return nullptr;
    }
  }

  // Step 9.a.
  if (!ISOResolveMonth(cx, &dateFields)) {
    return nullptr;
  }

  // Step 9.b.
  PlainDate result;
  if (!ISOMonthDayFromFields(cx, dateFields, overflow, &result)) {
    return nullptr;
  }

  // Step 11.
  Rooted<CalendarValue> calendar(cx, CalendarValue(calendarId));
  return CreateTemporalMonthDay(cx, result, calendar);
}

/**
 * CalendarMonthDayFromFields ( calendarRec, fields [ , options ] )
 */
static Wrapped<PlainMonthDayObject*> CalendarMonthDayFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar, Handle<JSObject*> fields,
    Handle<PlainObject*> maybeOptions) {
  MOZ_ASSERT(CalendarMethodsRecordHasLookedUp(
      calendar, CalendarMethod::MonthDayFromFields));

  // Step 1. (Not applicable in our implementation.)

  // Step 3. (Reordered)
  auto monthDayFromFields = calendar.monthDayFromFields();
  if (!monthDayFromFields) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());
    return BuiltinCalendarMonthDayFromFields(cx, calendarId, fields,
                                             maybeOptions);
  }

  // Step 2. (Inlined call to CalendarMethodsRecordCall.)

  Rooted<Value> monthDayFromFieldsFn(cx, ObjectValue(*monthDayFromFields));
  auto thisv = calendar.receiver().toObjectValue();
  Rooted<Value> rval(cx);

  FixedInvokeArgs<2> args(cx);
  args[0].setObject(*fields);
  if (maybeOptions) {
    args[1].setObject(*maybeOptions);
  } else {
    args[1].setUndefined();
  }

  if (!Call(cx, monthDayFromFieldsFn, thisv, args, &rval)) {
    return nullptr;
  }

  // Step 4.
  if (!rval.isObject() || !rval.toObject().canUnwrapAs<PlainMonthDayObject>()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, rval,
                     nullptr, "not a PlainMonthDay object");
    return nullptr;
  }

  // Step 5.
  return &rval.toObject();
}

/**
 * CalendarMonthDayFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainMonthDayObject*> js::temporal::CalendarMonthDayFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<PlainObject*> fields) {
  // Steps 1-4.
  return ::CalendarMonthDayFromFields(cx, calendar, fields, nullptr);
}

/**
 * CalendarMonthDayFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainMonthDayObject*> js::temporal::CalendarMonthDayFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<PlainMonthDayObject*> fields) {
  // Steps 1-4.
  return ::CalendarMonthDayFromFields(cx, calendar, fields, nullptr);
}

/**
 * CalendarMonthDayFromFields ( calendarRec, fields [ , options ] )
 */
Wrapped<PlainMonthDayObject*> js::temporal::CalendarMonthDayFromFields(
    JSContext* cx, Handle<CalendarRecord> calendar, Handle<PlainObject*> fields,
    Handle<PlainObject*> options) {
  // Steps 1-4.
  return ::CalendarMonthDayFromFields(cx, calendar, fields, options);
}

using PropertyHashSet = JS::GCHashSet<JS::PropertyKey>;
using PropertyVector = JS::StackGCVector<JS::PropertyKey>;

static bool SetFromList(JSContext* cx, const PropertyVector& keys,
                        PropertyHashSet& keysSet) {
  MOZ_ASSERT(keysSet.empty(), "expected an empty output hashset");

  if (!keysSet.reserve(keys.length())) {
    return false;
  }

  for (const auto& key : keys) {
    if (!keysSet.putNew(key)) {
      return false;
    }
  }
  return true;
}

/**
 * ISOFieldKeysToIgnore ( keys )
 */
static auto ISOFieldKeysToIgnore(mozilla::EnumSet<TemporalField> keys) {
  // Steps 1 and 2.a.
  auto ignoredKeys = keys;

  // Step 2.b.
  if (keys.contains(TemporalField::Month)) {
    ignoredKeys += TemporalField::MonthCode;
  }

  // Step 2.c.
  else if (keys.contains(TemporalField::MonthCode)) {
    ignoredKeys += TemporalField::Month;
  }

  // Steps 3-4.
  return ignoredKeys;
}

#ifdef DEBUG
static bool IsPlainDataObject(PlainObject* obj) {
  // [[Prototype]] is null.
  if (obj->staticPrototype() != nullptr) {
    return false;
  }

  // All properties are simple data properties.
  for (ShapePropertyIter<NoGC> iter(obj->shape()); !iter.done(); iter++) {
    if (iter->flags() != PropertyFlags::defaultDataPropFlags) {
      return false;
    }
  }
  return true;
}
#endif

/**
 * Temporal.Calendar.prototype.mergeFields ( fields, additionalFields )
 */
static PlainObject* BuiltinCalendarMergeFields(
    JSContext* cx, CalendarId calendarId, Handle<PlainObject*> fields,
    Handle<PlainObject*> additionalFields) {
  MOZ_ASSERT(IsPlainDataObject(fields));
  MOZ_ASSERT(IsPlainDataObject(additionalFields));

  // Steps 1-4. (Not applicable in our implementation.)

  // Steps 5-6.
  //
  // |additionalFields| contains no non-enumerable properties, so we don't need
  // to pass JSITER_HIDDEN.
  JS::RootedVector<PropertyKey> additionalKeys(cx);
  if (!GetPropertyKeys(cx, additionalFields, JSITER_OWNONLY | JSITER_SYMBOLS,
                       &additionalKeys)) {
    return nullptr;
  }

  // Steps 7-8.
  mozilla::EnumSet<TemporalField> additionalFieldKeys;
  for (const auto& additionalKey : additionalKeys) {
    auto field = ToTemporalField(cx, additionalKey);
    if (field) {
      additionalFieldKeys += *field;
    }
  }

  auto toIgnore = ISOFieldKeysToIgnore(additionalFieldKeys);
  MOZ_ASSERT(toIgnore.contains(additionalFieldKeys));

  Rooted<PropertyHashSet> overriddenKeys(cx, PropertyHashSet(cx));
  if (!SetFromList(cx, additionalKeys.get(), overriddenKeys.get())) {
    return nullptr;
  }

  auto additionalFieldsToIgnore = toIgnore - additionalFieldKeys;
  for (auto field : additionalFieldsToIgnore) {
    auto* fieldName = ToPropertyName(cx, field);
    if (!overriddenKeys.put(NameToId(fieldName))) {
      return nullptr;
    }
  }

  // Step 9.
  Rooted<PlainObject*> merged(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!merged) {
    return nullptr;
  }

  // Steps 10-11.
  //
  // See above why JSITER_HIDDEN isn't needed.
  JS::RootedVector<PropertyKey> fieldsKeys(cx);
  if (!GetPropertyKeys(cx, fields, JSITER_OWNONLY | JSITER_SYMBOLS,
                       &fieldsKeys)) {
    return nullptr;
  }

  // Step 12.
  Rooted<Value> propValue(cx);
  for (size_t i = 0; i < fieldsKeys.length(); i++) {
    Handle<PropertyKey> key = fieldsKeys[i];

    // Steps 12.a-b.
    if (overriddenKeys.has(key)) {
      if (!GetProperty(cx, additionalFields, additionalFields, key,
                       &propValue)) {
        return nullptr;
      }
    } else {
      if (!GetProperty(cx, fields, fields, key, &propValue)) {
        return nullptr;
      }
    }

    // Step 12.c.
    if (!propValue.isUndefined()) {
      if (!DefineDataProperty(cx, merged, key, propValue)) {
        return nullptr;
      }
    }
  }

  // Step 13.
  if (!CopyDataProperties(cx, merged, additionalFields)) {
    return nullptr;
  }

  // Step 14.
  return merged;
}

/**
 * CalendarMergeFields ( calendarRec, fields, additionalFields )
 */
JSObject* js::temporal::CalendarMergeFields(
    JSContext* cx, Handle<CalendarRecord> calendar, Handle<PlainObject*> fields,
    Handle<PlainObject*> additionalFields) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::MergeFields));

  // Step 2. (Reordered)
  auto mergeFields = calendar.mergeFields();
  if (!mergeFields) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());
    return BuiltinCalendarMergeFields(cx, calendarId, fields, additionalFields);
  }

  // Step 1. (Inlined call to CalendarMethodsRecordCall.)

  Rooted<Value> mergeFieldsFn(cx, ObjectValue(*mergeFields));
  auto thisv = calendar.receiver().toObjectValue();
  Rooted<Value> result(cx);

  FixedInvokeArgs<2> args(cx);
  args[0].setObject(*fields);
  args[1].setObject(*additionalFields);

  if (!Call(cx, mergeFieldsFn, thisv, args, &result)) {
    return nullptr;
  }

  // Steps 3-4.
  return RequireObject(cx, result);
}

/**
 * Temporal.Calendar.prototype.dateAdd ( date, duration [ , options ] )
 */
static bool BuiltinCalendarAdd(JSContext* cx, CalendarId calendarId,
                               const PlainDate& date,
                               const NormalizedDuration& duration,
                               Handle<JSObject*> options, PlainDate* result) {
  MOZ_ASSERT(IsValidISODate(date));
  MOZ_ASSERT(IsValidDuration(duration));

  // Steps 1-5. (Not applicable)

  // Step 6.
  auto overflow = TemporalOverflow::Constrain;
  if (options) {
    if (!GetTemporalOverflowOption(cx, options, &overflow)) {
      return false;
    }
  }

  // Step 7.
  const auto& timeDuration = duration.time;

  // Step 8.
  auto balanceResult = BalanceTimeDuration(timeDuration, TemporalUnit::Day);

  auto addDuration = DateDuration{
      duration.date.years,
      duration.date.months,
      duration.date.weeks,
      duration.date.days + balanceResult.days,
  };
  return AddISODate(cx, date, addDuration, overflow, result);
}

/**
 * Temporal.Calendar.prototype.dateAdd ( date, duration [ , options ] )
 */
static bool BuiltinCalendarAdd(JSContext* cx, CalendarId calendarId,
                               const PlainDate& date,
                               const DateDuration& duration,
                               Handle<JSObject*> options, PlainDate* result) {
  // Steps 1-5. (Not applicable)

  // Step 7. (Reordered)
  auto normalized = CreateNormalizedDurationRecord(duration, {});

  // Steps 6 and 8-10.
  return BuiltinCalendarAdd(cx, calendarId, date, normalized, options, result);
}

/**
 * Temporal.Calendar.prototype.dateAdd ( date, duration [ , options ] )
 */
static PlainDateObject* BuiltinCalendarAdd(JSContext* cx, CalendarId calendarId,
                                           const PlainDate& date,
                                           const DateDuration& duration,
                                           Handle<JSObject*> options) {
  // Steps 1-10.
  PlainDate result;
  if (!BuiltinCalendarAdd(cx, calendarId, date, duration, options, &result)) {
    return nullptr;
  }

  // Step 11.
  Rooted<CalendarValue> calendar(cx, CalendarValue(calendarId));
  return CreateTemporalDate(cx, result, calendar);
}

/**
 * Temporal.Calendar.prototype.dateAdd ( date, duration [ , options ] )
 */
static PlainDateObject* BuiltinCalendarAdd(JSContext* cx, CalendarId calendarId,
                                           const PlainDate& date,
                                           const Duration& duration,
                                           Handle<JSObject*> options) {
  // Steps 1-5. (Not applicable)

  // Step 7. (Reordered)
  auto normalized = CreateNormalizedDurationRecord(duration);

  // Steps 6 and 8-10.
  PlainDate result;
  if (!BuiltinCalendarAdd(cx, calendarId, date, normalized, options, &result)) {
    return nullptr;
  }

  // Step 11.
  Rooted<CalendarValue> calendar(cx, CalendarValue(calendarId));
  return CreateTemporalDate(cx, result, calendar);
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
static Wrapped<PlainDateObject*> CalendarDateAddSlow(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date,
    Handle<Wrapped<DurationObject*>> duration, Handle<JSObject*> options) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));
  MOZ_ASSERT(calendar.receiver().isObject());
  MOZ_ASSERT(calendar.dateAdd());

  // Step 1. (Not applicable).

  // Step 2. (Inlined call to CalendarMethodsRecordCall.)
  Rooted<JS::Value> dateAdd(cx, ObjectValue(*calendar.dateAdd()));
  auto thisv = calendar.receiver().toObjectValue();
  Rooted<Value> rval(cx);

  FixedInvokeArgs<3> args(cx);
  args[0].setObject(*date);
  args[1].setObject(*duration);
  if (options) {
    args[2].setObject(*options);
  } else {
    args[2].setUndefined();
  }

  if (!Call(cx, dateAdd, thisv, args, &rval)) {
    return nullptr;
  }

  // Step 3. (Not applicable)
  MOZ_ASSERT(!CalendarMethodsRecordIsBuiltin(calendar));

  // Step 4.
  if (!rval.isObject() || !rval.toObject().canUnwrapAs<PlainDateObject>()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, rval,
                     nullptr, "not a PlainDate object");
    return nullptr;
  }

  // Step 5.
  return &rval.toObject();
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
static Wrapped<PlainDateObject*> CalendarDateAdd(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date, const Duration& duration,
    Handle<JSObject*> options) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 1. (Not applicable).

  // Step 3. (Reordered)
  if (!calendar.dateAdd()) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());

    auto* unwrappedDate = date.unwrap(cx);
    if (!unwrappedDate) {
      return nullptr;
    }
    auto date = ToPlainDate(unwrappedDate);

    return BuiltinCalendarAdd(cx, calendarId, date, duration, options);
  }

  // Steps 2 and 4-5.
  Rooted<DurationObject*> durationObj(cx, CreateTemporalDuration(cx, duration));
  if (!durationObj) {
    return nullptr;
  }
  return CalendarDateAddSlow(cx, calendar, date, durationObj, options);
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
static Wrapped<PlainDateObject*> CalendarDateAdd(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date, const DateDuration& duration,
    Handle<JSObject*> options) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 1. (Not applicable).

  // Step 3. (Reordered)
  if (!calendar.dateAdd()) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());

    auto* unwrappedDate = date.unwrap(cx);
    if (!unwrappedDate) {
      return nullptr;
    }
    auto date = ToPlainDate(unwrappedDate);

    return BuiltinCalendarAdd(cx, calendarId, date, duration, options);
  }

  // Steps 2 and 4-5.
  Rooted<DurationObject*> durationObj(
      cx, CreateTemporalDuration(cx, duration.toDuration()));
  if (!durationObj) {
    return nullptr;
  }
  return CalendarDateAddSlow(cx, calendar, date, durationObj, options);
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
static Wrapped<PlainDateObject*> CalendarDateAdd(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date,
    Handle<Wrapped<DurationObject*>> duration, Handle<JSObject*> options) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 1. (Not applicable).

  // Step 3. (Reordered)
  if (!calendar.dateAdd()) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());

    auto* unwrappedDate = date.unwrap(cx);
    if (!unwrappedDate) {
      return nullptr;
    }
    auto date = ToPlainDate(unwrappedDate);

    auto* unwrappedDuration = duration.unwrap(cx);
    if (!unwrappedDuration) {
      return nullptr;
    }
    auto duration = ToDuration(unwrappedDuration);

    return BuiltinCalendarAdd(cx, calendarId, date, duration, options);
  }

  // Steps 2 and 4-5.
  return CalendarDateAddSlow(cx, calendar, date, duration, options);
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
static bool CalendarDateAdd(JSContext* cx, Handle<CalendarRecord> calendar,
                            Handle<Wrapped<PlainDateObject*>> date,
                            const DateDuration& duration,
                            Handle<JSObject*> options, PlainDate* result) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 1. (Not applicable).

  // Step 3. (Reordered)
  if (!calendar.dateAdd()) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());

    auto* unwrappedDate = date.unwrap(cx);
    if (!unwrappedDate) {
      return false;
    }
    auto date = ToPlainDate(unwrappedDate);

    return BuiltinCalendarAdd(cx, calendarId, date, duration, options, result);
  }

  // Steps 2 and 4-5.

  Rooted<DurationObject*> durationObj(
      cx, CreateTemporalDuration(cx, duration.toDuration()));
  if (!durationObj) {
    return false;
  }

  auto obj = CalendarDateAddSlow(cx, calendar, date, durationObj, options);
  if (!obj) {
    return false;
  }

  *result = ToPlainDate(&obj.unwrap());
  return true;
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
static bool CalendarDateAdd(JSContext* cx, Handle<CalendarRecord> calendar,
                            const PlainDate& date, const DateDuration& duration,
                            Handle<JSObject*> options, PlainDate* result) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateAdd));

  // Step 1. (Not applicable).

  // Step 3. (Reordered)
  if (!calendar.dateAdd()) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());
    return BuiltinCalendarAdd(cx, calendarId, date, duration, options, result);
  }

  // Steps 2 and 4-5.

  Rooted<PlainDateObject*> dateObj(
      cx, CreateTemporalDate(cx, date, calendar.receiver()));
  if (!dateObj) {
    return false;
  }

  Rooted<DurationObject*> durationObj(
      cx, CreateTemporalDuration(cx, duration.toDuration()));
  if (!durationObj) {
    return false;
  }

  auto obj = CalendarDateAddSlow(cx, calendar, dateObj, durationObj, options);
  if (!obj) {
    return false;
  }

  *result = ToPlainDate(&obj.unwrap());
  return true;
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
Wrapped<PlainDateObject*> js::temporal::CalendarDateAdd(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date, const Duration& duration,
    Handle<JSObject*> options) {
  // Step 1. (Not applicable).

  // Steps 2-5.
  return ::CalendarDateAdd(cx, calendar, date, duration, options);
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
Wrapped<PlainDateObject*> js::temporal::CalendarDateAdd(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date, const DateDuration& duration) {
  // Step 1.
  Handle<JSObject*> options = nullptr;

  // Steps 2-5.
  return ::CalendarDateAdd(cx, calendar, date, duration, options);
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
Wrapped<PlainDateObject*> js::temporal::CalendarDateAdd(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date,
    Handle<Wrapped<DurationObject*>> duration) {
  // Step 1.
  Handle<JSObject*> options = nullptr;

  // Steps 2-5.
  return ::CalendarDateAdd(cx, calendar, date, duration, options);
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
Wrapped<PlainDateObject*> js::temporal::CalendarDateAdd(
    JSContext* cx, Handle<CalendarRecord> calendar,
    Handle<Wrapped<PlainDateObject*>> date,
    Handle<Wrapped<DurationObject*>> duration, Handle<JSObject*> options) {
  // Step 1. (Not applicable).

  // Steps 2-5.
  return ::CalendarDateAdd(cx, calendar, date, duration, options);
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
bool js::temporal::CalendarDateAdd(JSContext* cx,
                                   Handle<CalendarRecord> calendar,
                                   const PlainDate& date,
                                   const DateDuration& duration,
                                   PlainDate* result) {
  // Step 1.
  Handle<JSObject*> options = nullptr;

  // Steps 2-5.
  return ::CalendarDateAdd(cx, calendar, date, duration, options, result);
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
bool js::temporal::CalendarDateAdd(JSContext* cx,
                                   Handle<CalendarRecord> calendar,
                                   const PlainDate& date,
                                   const DateDuration& duration,
                                   Handle<JSObject*> options,
                                   PlainDate* result) {
  // Step 1. (Not applicable)

  // Steps 2-5.
  return ::CalendarDateAdd(cx, calendar, date, duration, options, result);
}

/**
 * CalendarDateAdd ( calendarRec, date, duration [ , options ] )
 */
bool js::temporal::CalendarDateAdd(JSContext* cx,
                                   Handle<CalendarRecord> calendar,
                                   Handle<Wrapped<PlainDateObject*>> date,
                                   const DateDuration& duration,
                                   PlainDate* result) {
  // Step 1.
  Handle<JSObject*> options = nullptr;

  // Steps 2-5.
  return ::CalendarDateAdd(cx, calendar, date, duration, options, result);
}

/**
 * Temporal.Calendar.prototype.dateUntil ( one, two [ , options ] )
 */
static bool BuiltinCalendarDateUntil(JSContext* cx, CalendarId calendarId,
                                     const PlainDate& one, const PlainDate& two,
                                     TemporalUnit largestUnit,
                                     DateDuration* result) {
  // Steps 1-7. (Not applicable)

  // Step 8.
  *result = DifferenceISODate(one, two, largestUnit);
  return true;
}

/**
 * Temporal.Calendar.prototype.dateUntil ( one, two [ , options ] )
 */
static bool BuiltinCalendarDateUntil(JSContext* cx, CalendarId calendarId,
                                     Handle<Wrapped<PlainDateObject*>> one,
                                     Handle<Wrapped<PlainDateObject*>> two,
                                     TemporalUnit largestUnit,
                                     DateDuration* result) {
  MOZ_ASSERT(largestUnit <= TemporalUnit::Day);

  auto* unwrappedOne = one.unwrap(cx);
  if (!unwrappedOne) {
    return false;
  }
  auto dateOne = ToPlainDate(unwrappedOne);

  auto* unwrappedTwo = two.unwrap(cx);
  if (!unwrappedTwo) {
    return false;
  }
  auto dateTwo = ToPlainDate(unwrappedTwo);

  // Steps 1-9.
  return BuiltinCalendarDateUntil(cx, calendarId, dateOne, dateTwo, largestUnit,
                                  result);
}

static bool CalendarDateUntilSlow(JSContext* cx,
                                  Handle<CalendarRecord> calendar,
                                  Handle<Wrapped<PlainDateObject*>> one,
                                  Handle<Wrapped<PlainDateObject*>> two,
                                  TemporalUnit largestUnit,
                                  Handle<JSObject*> maybeOptions,
                                  DateDuration* result) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateUntil));
  MOZ_ASSERT(calendar.receiver().isObject());
  MOZ_ASSERT(calendar.dateUntil());

  Rooted<JSObject*> options(cx, maybeOptions);
  if (!options) {
    options = NewPlainObjectWithProto(cx, nullptr);
    if (!options) {
      return false;
    }
  }

  Rooted<Value> value(cx, StringValue(TemporalUnitToString(cx, largestUnit)));
  if (!DefineDataProperty(cx, options, cx->names().largestUnit, value)) {
    return false;
  }

  // Step 1. (Inlined call to CalendarMethodsRecordCall.)
  Rooted<JS::Value> dateUntil(cx, ObjectValue(*calendar.dateUntil()));
  auto thisv = calendar.receiver().toObjectValue();
  Rooted<Value> rval(cx);

  FixedInvokeArgs<3> args(cx);
  args[0].setObject(*one);
  args[1].setObject(*two);
  args[2].setObject(*options);

  if (!Call(cx, dateUntil, thisv, args, &rval)) {
    return false;
  }

  // Step 2. (Not applicable)
  MOZ_ASSERT(!CalendarMethodsRecordIsBuiltin(calendar));

  // Step 3.
  if (!rval.isObject() || !rval.toObject().canUnwrapAs<DurationObject>()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK, rval,
                     nullptr, "not a Duration object");
    return false;
  }

  // Step 4.
  auto duration = ToDuration(&rval.toObject().unwrapAs<DurationObject>());
  *result = duration.toDateDuration();
  return true;
}

static bool CalendarDateUntilSlow(JSContext* cx,
                                  Handle<CalendarRecord> calendar,
                                  const PlainDate& one, const PlainDate& two,
                                  TemporalUnit largestUnit,
                                  Handle<JSObject*> maybeOptions,
                                  DateDuration* result) {
  Rooted<PlainDateObject*> date1(
      cx, CreateTemporalDate(cx, one, calendar.receiver()));
  if (!date1) {
    return false;
  }

  Rooted<PlainDateObject*> date2(
      cx, CreateTemporalDate(cx, two, calendar.receiver()));
  if (!date2) {
    return false;
  }

  return CalendarDateUntilSlow(cx, calendar, date1, date2, largestUnit,
                               maybeOptions, result);
}

/**
 * CalendarDateUntil ( calendarRec, one, two, options )
 */
bool js::temporal::CalendarDateUntil(JSContext* cx,
                                     Handle<CalendarRecord> calendar,
                                     const PlainDate& one, const PlainDate& two,
                                     TemporalUnit largestUnit,
                                     DateDuration* result) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateUntil));
  MOZ_ASSERT(largestUnit <= TemporalUnit::Day);

  // Step 2. (Reordered)
  if (!calendar.dateUntil()) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());
    return BuiltinCalendarDateUntil(cx, calendarId, one, two, largestUnit,
                                    result);
  }

  // Steps 1 and 3-4.
  return CalendarDateUntilSlow(cx, calendar, one, two, largestUnit, nullptr,
                               result);
}

/**
 * CalendarDateUntil ( calendarRec, one, two, options )
 */
bool js::temporal::CalendarDateUntil(JSContext* cx,
                                     Handle<CalendarRecord> calendar,
                                     const PlainDate& one, const PlainDate& two,
                                     TemporalUnit largestUnit,
                                     Handle<PlainObject*> options,
                                     DateDuration* result) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateUntil));

  // As an optimization, our implementation only adds |largestUnit| to the
  // options object when taking the slow-path.
#ifdef DEBUG
  // The object must be extensible, otherwise we'd need to throw an error when
  // attempting to add the "largestUnit" property to a non-extensible object.
  MOZ_ASSERT(options->isExtensible());

  // Similarily, if there's an existing "largestUnit" property, this property
  // must be configurable.
  auto largestUnitProp = options->lookupPure(cx->names().largestUnit);
  MOZ_ASSERT_IF(largestUnitProp, largestUnitProp->configurable());
#endif

  // Step 2. (Reordered)
  if (!calendar.dateUntil()) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());
    return BuiltinCalendarDateUntil(cx, calendarId, one, two, largestUnit,
                                    result);
  }

  // Steps 1 and 3-4.
  return CalendarDateUntilSlow(cx, calendar, one, two, largestUnit, options,
                               result);
}

/**
 * CalendarDateUntil ( calendarRec, one, two, options )
 */
bool js::temporal::CalendarDateUntil(JSContext* cx,
                                     Handle<CalendarRecord> calendar,
                                     Handle<Wrapped<PlainDateObject*>> one,
                                     Handle<Wrapped<PlainDateObject*>> two,
                                     TemporalUnit largestUnit,
                                     DateDuration* result) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateUntil));
  MOZ_ASSERT(largestUnit <= TemporalUnit::Day);

  // Step 2. (Reordered)
  if (!calendar.dateUntil()) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());
    return BuiltinCalendarDateUntil(cx, calendarId, one, two, largestUnit,
                                    result);
  }

  // Steps 1 and 3-4.
  return CalendarDateUntilSlow(cx, calendar, one, two, largestUnit, nullptr,
                               result);
}

/**
 * CalendarDateUntil ( calendarRec, one, two, options )
 */
bool js::temporal::CalendarDateUntil(JSContext* cx,
                                     Handle<CalendarRecord> calendar,
                                     Handle<Wrapped<PlainDateObject*>> one,
                                     Handle<Wrapped<PlainDateObject*>> two,
                                     TemporalUnit largestUnit,
                                     Handle<PlainObject*> options,
                                     DateDuration* result) {
  MOZ_ASSERT(
      CalendarMethodsRecordHasLookedUp(calendar, CalendarMethod::DateUntil));

  // As an optimization, our implementation only adds |largestUnit| to the
  // options object when taking the slow-path.
#ifdef DEBUG
  // The object must be extensible, otherwise we'd need to throw an error when
  // attempting to add the "largestUnit" property to a non-extensible object.
  MOZ_ASSERT(options->isExtensible());

  // Similarily, if there's an existing "largestUnit" property, this property
  // must be configurable.
  auto largestUnitProp = options->lookupPure(cx->names().largestUnit);
  MOZ_ASSERT_IF(largestUnitProp, largestUnitProp->configurable());
#endif

  // Step 2. (Reordered)
  if (!calendar.dateUntil()) {
    auto calendarId = BuiltinCalendarId(calendar.receiver());
    return BuiltinCalendarDateUntil(cx, calendarId, one, two, largestUnit,
                                    result);
  }

  // Steps 1 and 3-4.
  return CalendarDateUntilSlow(cx, calendar, one, two, largestUnit, options,
                               result);
}

/**
 * CalendarEquals ( one, two )
 */
bool js::temporal::CalendarEquals(JSContext* cx, Handle<CalendarValue> one,
                                  Handle<CalendarValue> two, bool* equals) {
  // Step 1.
  if (one.isObject() && two.isObject() && one.toObject() == two.toObject()) {
    *equals = true;
    return true;
  }

  // Step 2.
  Rooted<JSLinearString*> calendarOne(cx,
                                      ToTemporalCalendarIdentifier(cx, one));
  if (!calendarOne) {
    return false;
  }

  // Step 3.
  JSLinearString* calendarTwo = ToTemporalCalendarIdentifier(cx, two);
  if (!calendarTwo) {
    return false;
  }

  // Steps 4-5.
  *equals = EqualStrings(calendarOne, calendarTwo);
  return true;
}

/**
 * CalendarEquals ( one, two )
 */
bool js::temporal::CalendarEqualsOrThrow(JSContext* cx,
                                         Handle<CalendarValue> one,
                                         Handle<CalendarValue> two) {
  // Step 1.
  if (one.isObject() && two.isObject() && one.toObject() == two.toObject()) {
    return true;
  }

  // Step 2.
  Rooted<JSLinearString*> calendarOne(cx,
                                      ToTemporalCalendarIdentifier(cx, one));
  if (!calendarOne) {
    return false;
  }

  // Step 3.
  JSLinearString* calendarTwo = ToTemporalCalendarIdentifier(cx, two);
  if (!calendarTwo) {
    return false;
  }

  // Steps 4-5.
  if (EqualStrings(calendarOne, calendarTwo)) {
    return true;
  }

  // Throw an error when the calendar identifiers don't match. Used when unequal
  // calendars throw a RangeError.
  if (auto charsOne = QuoteString(cx, calendarOne)) {
    if (auto charsTwo = QuoteString(cx, calendarTwo)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
                               charsOne.get(), charsTwo.get());
    }
  }
  return false;
}

/**
 * ConsolidateCalendars ( one, two )
 */
bool js::temporal::ConsolidateCalendars(JSContext* cx,
                                        Handle<CalendarValue> one,
                                        Handle<CalendarValue> two,
                                        MutableHandle<CalendarValue> result) {
  // Step 1.
  if (one.isObject() && two.isObject() && one.toObject() == two.toObject()) {
    result.set(two);
    return true;
  }

  // Step 2.
  Rooted<JSLinearString*> calendarOne(cx,
                                      ToTemporalCalendarIdentifier(cx, one));
  if (!calendarOne) {
    return false;
  }

  // Step 3.
  Rooted<JSLinearString*> calendarTwo(cx,
                                      ToTemporalCalendarIdentifier(cx, two));
  if (!calendarTwo) {
    return false;
  }

  // Step 4.
  if (EqualStrings(calendarOne, calendarTwo)) {
    result.set(two);
    return true;
  }

  // Step 5.
  if (StringEqualsLiteral(calendarOne, "iso8601")) {
    result.set(two);
    return true;
  }

  // Step 6.
  if (StringEqualsLiteral(calendarTwo, "iso8601")) {
    result.set(one);
    return true;
  }

  // Step 7.
  if (auto charsOne = QuoteString(cx, calendarOne)) {
    if (auto charsTwo = QuoteString(cx, calendarTwo)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
                               charsOne.get(), charsTwo.get());
    }
  }
  return false;
}

/**
 * Temporal.Calendar ( id )
 */
static bool CalendarConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Temporal.Calendar")) {
    return false;
  }

  // Step 2.
  if (!args.requireAtLeast(cx, "Temporal.Calendar", 1)) {
    return false;
  }

  if (!args[0].isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK, args[0],
                     nullptr, "not a string");
    return false;
  }

  Rooted<JSLinearString*> id(cx, args[0].toString()->ensureLinear(cx));
  if (!id) {
    return false;
  }

  // Step 3.
  CalendarId identifier;
  if (!::ToBuiltinCalendar(cx, id, &identifier)) {
    return false;
  }

  // Step 4.
  auto* calendar = CreateTemporalCalendar(cx, args, identifier);
  if (!calendar) {
    return false;
  }

  args.rval().setObject(*calendar);
  return true;
}

/**
 * Temporal.Calendar.from ( item )
 */
static bool Calendar_from(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<CalendarValue> calendar(cx);
  if (!ToTemporalCalendar(cx, args.get(0), &calendar)) {
    return false;
  }

  // Step 2.
  auto* obj = ToTemporalCalendarObject(cx, calendar);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * get Temporal.Calendar.prototype.id
 */
static bool Calendar_id(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();

  // Step 3.
  auto* str = ToTemporalCalendarIdentifier(cx, calendar->identifier());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * get Temporal.Calendar.prototype.id
 */
static bool Calendar_id(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_id>(cx, args);
}

/**
 * Temporal.Calendar.prototype.dateFromFields ( fields [ , options ] )
 */
static bool Calendar_dateFromFields(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  Rooted<JSObject*> fields(
      cx, RequireObjectArg(cx, "fields", "dateFromFields", args.get(0)));
  if (!fields) {
    return false;
  }

  // Step 4.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    options = RequireObjectArg(cx, "options", "dateFromFields", args[1]);
    if (!options) {
      return false;
    }
  }

  // Steps 5-11.
  auto* obj = BuiltinCalendarDateFromFields(cx, calendarId, fields, options);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.Calendar.prototype.dateFromFields ( fields [ , options ] )
 */
static bool Calendar_dateFromFields(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_dateFromFields>(cx, args);
}

/**
 * Temporal.Calendar.prototype.yearMonthFromFields ( fields [ , options ] )
 */
static bool Calendar_yearMonthFromFields(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  Rooted<JSObject*> fields(
      cx, RequireObjectArg(cx, "fields", "yearMonthFromFields", args.get(0)));
  if (!fields) {
    return false;
  }

  // Step 4.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    options = RequireObjectArg(cx, "options", "yearMonthFromFields", args[1]);
    if (!options) {
      return false;
    }
  }

  // Steps 5-11.
  auto* obj =
      BuiltinCalendarYearMonthFromFields(cx, calendarId, fields, options);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.Calendar.prototype.yearMonthFromFields ( fields [ , options ] )
 */
static bool Calendar_yearMonthFromFields(JSContext* cx, unsigned argc,
                                         Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_yearMonthFromFields>(cx,
                                                                        args);
}

/**
 * Temporal.Calendar.prototype.monthDayFromFields ( fields [ , options ] )
 */
static bool Calendar_monthDayFromFields(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  Rooted<JSObject*> fields(
      cx, RequireObjectArg(cx, "fields", "monthDayFromFields", args.get(0)));
  if (!fields) {
    return false;
  }

  // Step 4.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(1)) {
    options = RequireObjectArg(cx, "options", "monthDayFromFields", args[1]);
    if (!options) {
      return false;
    }
  }

  // Steps 5-11.
  auto* obj =
      BuiltinCalendarMonthDayFromFields(cx, calendarId, fields, options);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.Calendar.prototype.monthDayFromFields ( fields [ , options ] )
 */
static bool Calendar_monthDayFromFields(JSContext* cx, unsigned argc,
                                        Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_monthDayFromFields>(cx,
                                                                       args);
}

/**
 * Temporal.Calendar.prototype.dateAdd ( date, duration [ , options ] )
 */
static bool Calendar_dateAdd(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToTemporalDate(cx, args.get(0), &date)) {
    return false;
  }

  // Step 4.
  Duration duration;
  if (!ToTemporalDuration(cx, args.get(1), &duration)) {
    return false;
  }

  // Step 5.
  Rooted<JSObject*> options(cx);
  if (args.hasDefined(2)) {
    options = RequireObjectArg(cx, "options", "dateAdd", args[2]);
    if (!options) {
      return false;
    }
  }

  // Steps 6-11.
  auto* obj = BuiltinCalendarAdd(cx, calendarId, date, duration, options);
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.Calendar.prototype.dateAdd ( date, duration [ , options ] )
 */
static bool Calendar_dateAdd(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_dateAdd>(cx, args);
}

/**
 * Temporal.Calendar.prototype.dateUntil ( one, two [ , options ] )
 */
static bool Calendar_dateUntil(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate one;
  if (!ToTemporalDate(cx, args.get(0), &one)) {
    return false;
  }

  // Step 4.
  PlainDate two;
  if (!ToTemporalDate(cx, args.get(1), &two)) {
    return false;
  }

  // Steps 5-7.
  auto largestUnit = TemporalUnit::Day;
  if (args.hasDefined(2)) {
    // Step 5.
    Rooted<JSObject*> options(
        cx, RequireObjectArg(cx, "options", "dateUntil", args[2]));
    if (!options) {
      return false;
    }

    // Steps 6-7.
    if (!GetTemporalUnitValuedOption(cx, options, TemporalUnitKey::LargestUnit,
                                     TemporalUnitGroup::Date, &largestUnit)) {
      return false;
    }
  }

  // Steps 8-9.
  DateDuration duration;
  if (!BuiltinCalendarDateUntil(cx, calendarId, one, two, largestUnit,
                                &duration)) {
    return false;
  }

  // Step 10.
  auto* obj = CreateTemporalDuration(cx, duration.toDuration());
  if (!obj) {
    return false;
  }

  args.rval().setObject(*obj);
  return true;
}

/**
 * Temporal.Calendar.prototype.dateUntil ( one, two [ , options ] )
 */
static bool Calendar_dateUntil(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_dateUntil>(cx, args);
}

/**
 * Temporal.Calendar.prototype.era ( temporalDateLike )
 */
static bool Calendar_era(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToPlainDate<PlainDateObject, PlainDateTimeObject, PlainYearMonthObject>(
          cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-5.
  return BuiltinCalendarEra(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.era ( temporalDateLike )
 */
static bool Calendar_era(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_era>(cx, args);
}

/**
 * Temporal.Calendar.prototype.eraYear ( temporalDateLike )
 */
static bool Calendar_eraYear(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToPlainDate<PlainDateObject, PlainDateTimeObject, PlainYearMonthObject>(
          cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-7.
  return BuiltinCalendarEraYear(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.eraYear ( temporalDateLike )
 */
static bool Calendar_eraYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_eraYear>(cx, args);
}

/**
 * Temporal.Calendar.prototype.year ( temporalDateLike )
 */
static bool Calendar_year(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToPlainDate<PlainDateObject, PlainDateTimeObject, PlainYearMonthObject>(
          cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarYear(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.year ( temporalDateLike )
 */
static bool Calendar_year(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_year>(cx, args);
}

/**
 * Temporal.Calendar.prototype.month ( temporalDateLike )
 */
static bool Calendar_month(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  Handle<Value> temporalDateLike = args.get(0);
  if (temporalDateLike.isObject() &&
      temporalDateLike.toObject().canUnwrapAs<PlainMonthDayObject>()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_SEARCH_STACK,
                     temporalDateLike, nullptr, "a PlainMonthDay object");
    return false;
  }

  // Step 4.
  PlainDate date;
  if (!ToPlainDate<PlainDateObject, PlainDateTimeObject, PlainYearMonthObject>(
          cx, temporalDateLike, &date)) {
    return false;
  }

  // Steps 5-7.
  return BuiltinCalendarMonth(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.month ( temporalDateLike )
 */
static bool Calendar_month(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_month>(cx, args);
}

/**
 * Temporal.Calendar.prototype.monthCode ( temporalDateLike )
 */
static bool Calendar_monthCode(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToPlainDate<PlainDateObject, PlainDateTimeObject, PlainMonthDayObject,
                   PlainYearMonthObject>(cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarMonthCode(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.monthCode ( temporalDateLike )
 */
static bool Calendar_monthCode(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_monthCode>(cx, args);
}

/**
 * Temporal.Calendar.prototype.day ( temporalDateLike )
 */
static bool Calendar_day(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToPlainDate<PlainDateObject, PlainDateTimeObject, PlainMonthDayObject>(
          cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarDay(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.day ( temporalDateLike )
 */
static bool Calendar_day(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_day>(cx, args);
}

/**
 * Temporal.Calendar.prototype.dayOfWeek ( temporalDateLike )
 */
static bool Calendar_dayOfWeek(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToTemporalDate(cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarDayOfWeek(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.dayOfWeek ( temporalDateLike )
 */
static bool Calendar_dayOfWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_dayOfWeek>(cx, args);
}

/**
 * Temporal.Calendar.prototype.dayOfYear ( temporalDateLike )
 */
static bool Calendar_dayOfYear(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToTemporalDate(cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarDayOfYear(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.dayOfYear ( temporalDateLike )
 */
static bool Calendar_dayOfYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_dayOfYear>(cx, args);
}

/**
 * Temporal.Calendar.prototype.weekOfYear ( temporalDateLike )
 */
static bool Calendar_weekOfYear(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToTemporalDate(cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarWeekOfYear(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.weekOfYear ( temporalDateLike )
 */
static bool Calendar_weekOfYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_weekOfYear>(cx, args);
}

/**
 * Temporal.Calendar.prototype.yearOfWeek ( temporalDateLike )
 */
static bool Calendar_yearOfWeek(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToTemporalDate(cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarYearOfWeek(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.yearOfWeek ( temporalDateLike )
 */
static bool Calendar_yearOfWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_yearOfWeek>(cx, args);
}

/**
 * Temporal.Calendar.prototype.daysInWeek ( temporalDateLike )
 */
static bool Calendar_daysInWeek(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToTemporalDate(cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarDaysInWeek(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.daysInWeek ( temporalDateLike )
 */
static bool Calendar_daysInWeek(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_daysInWeek>(cx, args);
}

/**
 * Temporal.Calendar.prototype.daysInMonth ( temporalDateLike )
 */
static bool Calendar_daysInMonth(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToPlainDate<PlainDateObject, PlainDateTimeObject, PlainYearMonthObject>(
          cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarDaysInMonth(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.daysInMonth ( temporalDateLike )
 */
static bool Calendar_daysInMonth(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_daysInMonth>(cx, args);
}

/**
 * Temporal.Calendar.prototype.daysInYear ( temporalDateLike )
 */
static bool Calendar_daysInYear(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToPlainDate<PlainDateObject, PlainDateTimeObject, PlainYearMonthObject>(
          cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarDaysInYear(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.daysInYear ( temporalDateLike )
 */
static bool Calendar_daysInYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_daysInYear>(cx, args);
}

/**
 * Temporal.Calendar.prototype.monthsInYear ( temporalDateLike )
 */
static bool Calendar_monthsInYear(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToPlainDate<PlainDateObject, PlainDateTimeObject, PlainYearMonthObject>(
          cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarMonthsInYear(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.monthsInYear ( temporalDateLike )
 */
static bool Calendar_monthsInYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_monthsInYear>(cx, args);
}

/**
 * Temporal.Calendar.prototype.inLeapYear ( temporalDateLike )
 */
static bool Calendar_inLeapYear(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  PlainDate date;
  if (!ToPlainDate<PlainDateObject, PlainDateTimeObject, PlainYearMonthObject>(
          cx, args.get(0), &date)) {
    return false;
  }

  // Steps 4-6.
  return BuiltinCalendarInLeapYear(cx, calendarId, date, args.rval());
}

/**
 * Temporal.Calendar.prototype.inLeapYear ( temporalDateLike )
 */
static bool Calendar_inLeapYear(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_inLeapYear>(cx, args);
}

/**
 * Temporal.Calendar.prototype.fields ( fields )
 */
static bool Calendar_fields(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Steps 3-9.
  return BuiltinCalendarFields(cx, calendarId, args.get(0), args.rval());
}

/**
 * Temporal.Calendar.prototype.fields ( fields )
 */
static bool Calendar_fields(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_fields>(cx, args);
}

/**
 * Temporal.Calendar.prototype.mergeFields ( fields, additionalFields )
 */
static bool Calendar_mergeFields(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();
  auto calendarId = calendar->identifier();

  // Step 3.
  Rooted<JSObject*> fields(cx, JS::ToObject(cx, args.get(0)));
  if (!fields) {
    return false;
  }

  Rooted<PlainObject*> fieldsCopy(
      cx, SnapshotOwnPropertiesIgnoreUndefined(cx, fields));
  if (!fieldsCopy) {
    return false;
  }

  // Step 4.
  Rooted<JSObject*> additionalFields(cx, JS::ToObject(cx, args.get(1)));
  if (!additionalFields) {
    return false;
  }

  Rooted<PlainObject*> additionalFieldsCopy(
      cx, SnapshotOwnPropertiesIgnoreUndefined(cx, additionalFields));
  if (!additionalFieldsCopy) {
    return false;
  }

  // Steps 5-13.
  auto* merged = BuiltinCalendarMergeFields(cx, calendarId, fieldsCopy,
                                            additionalFieldsCopy);
  if (!merged) {
    return false;
  }

  // Step 14.
  args.rval().setObject(*merged);
  return true;
}

/**
 * Temporal.Calendar.prototype.mergeFields ( fields, additionalFields )
 */
static bool Calendar_mergeFields(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_mergeFields>(cx, args);
}

/**
 * Temporal.Calendar.prototype.toString ( )
 */
static bool Calendar_toString(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();

  // Step 3.
  auto* str = ToTemporalCalendarIdentifier(cx, calendar->identifier());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.Calendar.prototype.toString ( )
 */
static bool Calendar_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_toString>(cx, args);
}

/**
 * Temporal.Calendar.prototype.toJSON ( )
 */
static bool Calendar_toJSON(JSContext* cx, const CallArgs& args) {
  auto* calendar = &args.thisv().toObject().as<CalendarObject>();

  // Step 3.
  auto* str = ToTemporalCalendarIdentifier(cx, calendar->identifier());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

/**
 * Temporal.Calendar.prototype.toJSON ( )
 */
static bool Calendar_toJSON(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsCalendar, Calendar_toJSON>(cx, args);
}

const JSClass CalendarObject::class_ = {
    "Temporal.Calendar",
    JSCLASS_HAS_RESERVED_SLOTS(CalendarObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Calendar),
    JS_NULL_CLASS_OPS,
    &CalendarObject::classSpec_,
};

const JSClass& CalendarObject::protoClass_ = PlainObject::class_;

static const JSFunctionSpec Calendar_methods[] = {
    JS_FN("from", Calendar_from, 1, 0),
    JS_FS_END,
};

static const JSFunctionSpec Calendar_prototype_methods[] = {
    JS_FN("dateFromFields", Calendar_dateFromFields, 1, 0),
    JS_FN("yearMonthFromFields", Calendar_yearMonthFromFields, 1, 0),
    JS_FN("monthDayFromFields", Calendar_monthDayFromFields, 1, 0),
    JS_FN("dateAdd", Calendar_dateAdd, 2, 0),
    JS_FN("dateUntil", Calendar_dateUntil, 2, 0),
    JS_FN("era", Calendar_era, 1, 0),
    JS_FN("eraYear", Calendar_eraYear, 1, 0),
    JS_FN("year", Calendar_year, 1, 0),
    JS_FN("month", Calendar_month, 1, 0),
    JS_FN("monthCode", Calendar_monthCode, 1, 0),
    JS_FN("day", Calendar_day, 1, 0),
    JS_FN("dayOfWeek", Calendar_dayOfWeek, 1, 0),
    JS_FN("dayOfYear", Calendar_dayOfYear, 1, 0),
    JS_FN("weekOfYear", Calendar_weekOfYear, 1, 0),
    JS_FN("yearOfWeek", Calendar_yearOfWeek, 1, 0),
    JS_FN("daysInWeek", Calendar_daysInWeek, 1, 0),
    JS_FN("daysInMonth", Calendar_daysInMonth, 1, 0),
    JS_FN("daysInYear", Calendar_daysInYear, 1, 0),
    JS_FN("monthsInYear", Calendar_monthsInYear, 1, 0),
    JS_FN("inLeapYear", Calendar_inLeapYear, 1, 0),
    JS_FN("fields", Calendar_fields, 1, 0),
    JS_FN("mergeFields", Calendar_mergeFields, 2, 0),
    JS_FN("toString", Calendar_toString, 0, 0),
    JS_FN("toJSON", Calendar_toJSON, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec Calendar_prototype_properties[] = {
    JS_PSG("id", Calendar_id, 0),
    JS_STRING_SYM_PS(toStringTag, "Temporal.Calendar", JSPROP_READONLY),
    JS_PS_END,
};

const ClassSpec CalendarObject::classSpec_ = {
    GenericCreateConstructor<CalendarConstructor, 1, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<CalendarObject>,
    Calendar_methods,
    nullptr,
    Calendar_prototype_methods,
    Calendar_prototype_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

struct MOZ_STACK_CLASS CalendarNameAndNative final {
  PropertyName* name;
  JSNative native;
};

static CalendarNameAndNative GetCalendarNameAndNative(JSContext* cx,
                                                      CalendarField fieldName) {
  switch (fieldName) {
    case CalendarField::Year:
      return {cx->names().year, Calendar_year};
    case CalendarField::Month:
      return {cx->names().month, Calendar_month};
    case CalendarField::MonthCode:
      return {cx->names().monthCode, Calendar_monthCode};
    case CalendarField::Day:
      return {cx->names().day, Calendar_day};
  }
  MOZ_CRASH("invalid temporal field name");
}

bool js::temporal::IsBuiltinAccess(
    JSContext* cx, Handle<CalendarObject*> calendar,
    std::initializer_list<CalendarField> fieldNames) {
  // Don't optimize when the object has any own properties which may shadow the
  // built-in methods.
  if (!calendar->empty()) {
    return false;
  }

  JSObject* proto = cx->global()->maybeGetPrototype(JSProto_Calendar);

  // Don't attempt to optimize when the class isn't yet initialized.
  if (!proto) {
    return false;
  }

  // Don't optimize when the prototype isn't the built-in prototype.
  if (calendar->staticPrototype() != proto) {
    return false;
  }

  auto* nproto = &proto->as<NativeObject>();
  for (auto fieldName : fieldNames) {
    auto [name, native] = GetCalendarNameAndNative(cx, fieldName);
    auto prop = nproto->lookupPure(name);

    // Return if the property isn't a data property.
    if (!prop || !prop->isDataProperty()) {
      return false;
    }

    // Return if the property isn't the initial method.
    if (!IsNativeFunction(nproto->getSlot(prop->slot()), native)) {
      return false;
    }
  }

  // TODO: Pass accessor list from caller to avoid excessive checks.

  // Additionally check the various calendar fields operations.
  for (const auto& [name, native] : (CalendarNameAndNative[]){
           {cx->names().fields, Calendar_fields},
           {cx->names().mergeFields, Calendar_mergeFields},
           {cx->names().dateFromFields, Calendar_dateFromFields},
           {cx->names().monthDayFromFields, Calendar_monthDayFromFields},
           {cx->names().yearMonthFromFields, Calendar_yearMonthFromFields},
       }) {
    auto prop = nproto->lookupPure(name);

    // Return if the property isn't a data property.
    if (!prop || !prop->isDataProperty()) {
      return false;
    }

    // Return if the property isn't the initial method.
    if (!IsNativeFunction(nproto->getSlot(prop->slot()), native)) {
      return false;
    }
  }

  // CalendarFields observably uses array iteration.
  bool arrayIterationSane;
  if (!IsArrayIterationSane(cx, &arrayIterationSane)) {
    cx->recoverFromOutOfMemory();
    return false;
  }
  if (!arrayIterationSane) {
    return false;
  }

  // Success! The access can be optimized.
  return true;
}
