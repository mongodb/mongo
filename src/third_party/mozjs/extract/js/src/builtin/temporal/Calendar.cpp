/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/Calendar.h"

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/EnumSet.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/intl/ICU4XGeckoDataProvider.h"
#include "mozilla/intl/Locale.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/Result.h"
#include "mozilla/ResultVariant.h"
#include "mozilla/Span.h"
#include "mozilla/TextUtils.h"
#include "mozilla/UniquePtr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <stddef.h>
#include <stdint.h>
#include <utility>

#include "diplomat_runtime.h"
#include "ICU4XAnyCalendarKind.h"
#include "ICU4XCalendar.h"
#include "ICU4XDate.h"
#include "ICU4XError.h"
#include "ICU4XIsoDate.h"
#include "ICU4XIsoWeekday.h"
#include "ICU4XWeekCalculator.h"
#include "ICU4XWeekRelativeUnit.h"

#include "jsnum.h"
#include "jstypes.h"
#include "NamespaceImports.h"

#include "builtin/temporal/CalendarFields.h"
#include "builtin/temporal/Crash.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/Era.h"
#include "builtin/temporal/MonthCode.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainMonthDay.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/PlainYearMonth.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TemporalRoundingMode.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "gc/Barrier.h"
#include "gc/GCEnum.h"
#include "js/AllocPolicy.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/Printer.h"
#include "js/RootingAPI.h"
#include "js/TracingAPI.h"
#include "js/Value.h"
#include "js/Vector.h"
#include "util/Text.h"
#include "vm/BytecodeUtil.h"
#include "vm/Compartment.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/StringType.h"

#include "vm/Compartment-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/ObjectOperations-inl.h"

using namespace js;
using namespace js::temporal;

void js::temporal::CalendarValue::trace(JSTracer* trc) {
  TraceRoot(trc, &value_, "CalendarValue::value");
}

bool js::temporal::WrapCalendarValue(JSContext* cx,
                                     MutableHandle<JS::Value> calendar) {
  MOZ_ASSERT(calendar.isInt32());
  return cx->compartment()->wrap(cx, calendar);
}

/**
 * IsISOLeapYear ( year )
 */
static constexpr bool IsISOLeapYear(int32_t year) {
  // Steps 1-5.
  return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

/**
 * ISODaysInYear ( year )
 */
static int32_t ISODaysInYear(int32_t year) {
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
 * ISODayOfWeek ( isoDate )
 */
static int32_t ISODayOfWeek(const ISODate& isoDate) {
  MOZ_ASSERT(ISODateWithinLimits(isoDate));

  // Step 1.
  int32_t day = MakeDay(isoDate);

  // Step 2.
  int32_t dayOfWeek = WeekDay(day);

  // Steps 3-4.
  return dayOfWeek != 0 ? dayOfWeek : 7;
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
 * ISODayOfYear ( isoDate )
 */
static int32_t ISODayOfYear(const ISODate& isoDate) {
  MOZ_ASSERT(ISODateWithinLimits(isoDate));

  const auto& [year, month, day] = isoDate;

  // First day of month arrays for non-leap and leap years.
  constexpr decltype(FirstDayOfMonth(0)) firstDayOfMonth[2] = {
      FirstDayOfMonth(1), FirstDayOfMonth(0)};

  // Steps 1-2.
  //
  // Instead of first computing the date and then using DayWithinYear to map the
  // date to the day within the year, directly lookup the first day of the month
  // and then add the additional days.
  return firstDayOfMonth[IsISOLeapYear(year)][month - 1] + day;
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
static int64_t MakeTime(const Time& time) {
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
int32_t js::temporal::MakeDay(const ISODate& date) {
  MOZ_ASSERT(ISODateWithinLimits(date));

  return DayFromYear(date.year) + ISODayOfYear(date) - 1;
}

/**
 * 21.4.1.13 MakeDate ( day, time )
 */
int64_t js::temporal::MakeDate(const ISODateTime& dateTime) {
  MOZ_ASSERT(ISODateTimeWithinLimits(dateTime));

  // Step 1 (Not applicable).

  // Steps 2-3.
  int64_t tv = MakeDay(dateTime.date) * ToMilliseconds(TemporalUnit::Day) +
               MakeTime(dateTime.time);

  // Step 4.
  return tv;
}

struct YearWeek final {
  int32_t year = 0;
  int32_t week = 0;
};

/**
 * ISOWeekOfYear ( isoDate )
 */
static YearWeek ISOWeekOfYear(const ISODate& isoDate) {
  MOZ_ASSERT(ISODateWithinLimits(isoDate));

  // Step 1.
  int32_t year = isoDate.year;

  // Step 2-7. (Not applicable in our implementation.)

  // Steps 8-9.
  int32_t dayOfYear = ISODayOfYear(isoDate);
  int32_t dayOfWeek = ISODayOfWeek(isoDate);

  // Step 10.
  int32_t week = (10 + dayOfYear - dayOfWeek) / 7;
  MOZ_ASSERT(0 <= week && week <= 53);

  // An ISO year has 53 weeks if the year starts on a Thursday or if it's a
  // leap year which starts on a Wednesday.
  auto isLongYear = [](int32_t year) {
    int32_t startOfYear = ISODayOfWeek({year, 1, 1});
    return startOfYear == 4 || (startOfYear == 3 && IsISOLeapYear(year));
  };

  // Step 11.
  //
  // Part of last year's last week, which is either week 52 or week 53.
  if (week == 0) {
    return {year - 1, 52 + int32_t(isLongYear(year - 1))};
  }

  // Step 12.
  //
  // Part of next year's first week if the current year isn't a long year.
  if (week == 53 && !isLongYear(year)) {
    return {year + 1, 1};
  }

  // Step 13.
  return {year, week};
}

/**
 * ToTemporalCalendarIdentifier ( calendarSlotValue )
 */
std::string_view js::temporal::CalendarIdentifier(CalendarId calendarId) {
  switch (calendarId) {
    case CalendarId::ISO8601:
      return "iso8601";
    case CalendarId::Buddhist:
      return "buddhist";
    case CalendarId::Chinese:
      return "chinese";
    case CalendarId::Coptic:
      return "coptic";
    case CalendarId::Dangi:
      return "dangi";
    case CalendarId::Ethiopian:
      return "ethiopic";
    case CalendarId::EthiopianAmeteAlem:
      return "ethioaa";
    case CalendarId::Gregorian:
      return "gregory";
    case CalendarId::Hebrew:
      return "hebrew";
    case CalendarId::Indian:
      return "indian";
    case CalendarId::Islamic:
      return "islamic";
    case CalendarId::IslamicCivil:
      return "islamic-civil";
    case CalendarId::IslamicRGSA:
      return "islamic-rgsa";
    case CalendarId::IslamicTabular:
      return "islamic-tbla";
    case CalendarId::IslamicUmmAlQura:
      return "islamic-umalqura";
    case CalendarId::Japanese:
      return "japanese";
    case CalendarId::Persian:
      return "persian";
    case CalendarId::ROC:
      return "roc";
  }
  MOZ_CRASH("invalid calendar id");
}

class MOZ_STACK_CLASS AsciiLowerCaseChars final {
  static constexpr size_t InlineCapacity = 24;

  Vector<char, InlineCapacity> chars_;

 public:
  explicit AsciiLowerCaseChars(JSContext* cx) : chars_(cx) {}

  operator mozilla::Span<const char>() const {
    return mozilla::Span<const char>{chars_};
  }

  [[nodiscard]] bool init(JSLinearString* str) {
    MOZ_ASSERT(StringIsAscii(str));

    if (!chars_.resize(str->length())) {
      return false;
    }

    CopyChars(reinterpret_cast<JS::Latin1Char*>(chars_.begin()), *str);

    mozilla::intl::AsciiToLowerCase(chars_.begin(), chars_.length(),
                                    chars_.begin());

    return true;
  }
};

/**
 * CanonicalizeCalendar ( id )
 */
bool js::temporal::CanonicalizeCalendar(JSContext* cx, Handle<JSString*> id,
                                        MutableHandle<CalendarValue> result) {
  Rooted<JSLinearString*> linear(cx, id->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Steps 1-3.
  do {
    if (!StringIsAscii(linear) || linear->empty()) {
      break;
    }

    AsciiLowerCaseChars lowerCaseChars(cx);
    if (!lowerCaseChars.init(linear)) {
      return false;
    }
    mozilla::Span<const char> id = lowerCaseChars;

    // Reject invalid types before trying to resolve aliases.
    if (mozilla::intl::LocaleParser::CanParseUnicodeExtensionType(id).isErr()) {
      break;
    }

    // Resolve calendar aliases.
    static constexpr auto key = mozilla::MakeStringSpan("ca");
    if (const char* replacement =
            mozilla::intl::Locale::ReplaceUnicodeExtensionType(key, id)) {
      id = mozilla::MakeStringSpan(replacement);
    }

    // Step 1.
    static constexpr auto& calendars = AvailableCalendars();

    // Steps 2-3.
    for (auto identifier : calendars) {
      if (id == mozilla::Span{CalendarIdentifier(identifier)}) {
        result.set(CalendarValue(identifier));
        return true;
      }
    }
  } while (false);

  if (auto chars = QuoteString(cx, linear)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_TEMPORAL_CALENDAR_INVALID_ID, chars.get());
  }
  return false;
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
 * ToTemporalCalendarSlotValue ( temporalCalendarLike )
 */
bool js::temporal::ToTemporalCalendar(JSContext* cx,
                                      Handle<Value> temporalCalendarLike,
                                      MutableHandle<CalendarValue> result) {
  // Step 1.
  if (temporalCalendarLike.isObject()) {
    Rooted<JSObject*> obj(cx, &temporalCalendarLike.toObject());

    // Step 1.a.
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
  }

  // Step 2.
  if (!temporalCalendarLike.isString()) {
    ReportValueError(cx, JSMSG_UNEXPECTED_TYPE, JSDVG_IGNORE_STACK,
                     temporalCalendarLike, nullptr, "not a string");
    return false;
  }
  Rooted<JSString*> str(cx, temporalCalendarLike.toString());

  // Step 3.
  Rooted<JSLinearString*> id(cx, ParseTemporalCalendarString(cx, str));
  if (!id) {
    return false;
  }

  // Step 4.
  return CanonicalizeCalendar(cx, id, result);
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
  if (calendarValue.isUndefined()) {
    result.set(CalendarValue(CalendarId::ISO8601));
    return true;
  }

  // Step 4.
  return ToTemporalCalendar(cx, calendarValue, result);
}

static inline bool DayOfMonthCanBeZero(CalendarId calendarId) {
  // Workaround when day-of-month returns zero.
  //
  // See <https://github.com/unicode-org/icu4x/issues/5069>.
  static constexpr mozilla::EnumSet<CalendarId> calendars{
      CalendarId::Islamic,
      CalendarId::IslamicRGSA,
      CalendarId::IslamicUmmAlQura,
  };
  return calendars.contains(calendarId);
}

static inline int32_t OrdinalMonth(CalendarId calendarId,
                                   const capi::ICU4XDate* date) {
  int32_t month = capi::ICU4XDate_ordinal_month(date);
  MOZ_ASSERT(month > 0);

  if (DayOfMonthCanBeZero(calendarId)) {
    // If |dayOfMonth| is zero, interpret as last day of previous month.
    int32_t dayOfMonth = capi::ICU4XDate_day_of_month(date);
    if (dayOfMonth == 0) {
      MOZ_ASSERT(month > 1);
      month -= 1;
    }
  }

  return month;
}

static inline int32_t DayOfMonth(CalendarId calendarId,
                                 const capi::ICU4XDate* date) {
  int32_t dayOfMonth = capi::ICU4XDate_day_of_month(date);

  if (DayOfMonthCanBeZero(calendarId)) {
    // If |dayOfMonth| is zero, interpret as last day of previous month.
    if (dayOfMonth == 0) {
      MOZ_ASSERT(CalendarDaysInMonth(calendarId).second == 30);
      dayOfMonth = 30;
    }
  }

  MOZ_ASSERT(dayOfMonth > 0);
  return dayOfMonth;
}

static inline int32_t DayOfYear(const capi::ICU4XDate* date) {
  int32_t dayOfYear = capi::ICU4XDate_day_of_year(date);
  MOZ_ASSERT(dayOfYear > 0);
  return dayOfYear;
}

static inline int32_t DaysInMonth(const capi::ICU4XDate* date) {
  int32_t daysInMonth = capi::ICU4XDate_days_in_month(date);
  MOZ_ASSERT(daysInMonth > 0);
  return daysInMonth;
}

static inline int32_t DaysInYear(const capi::ICU4XDate* date) {
  int32_t daysInYear = capi::ICU4XDate_days_in_year(date);
  MOZ_ASSERT(daysInYear > 0);
  return daysInYear;
}

static inline int32_t MonthsInYear(const capi::ICU4XDate* date) {
  int32_t monthsInYear = capi::ICU4XDate_months_in_year(date);
  MOZ_ASSERT(monthsInYear > 0);
  return monthsInYear;
}

static auto ToAnyCalendarKind(CalendarId id) {
  switch (id) {
    case CalendarId::ISO8601:
      return capi::ICU4XAnyCalendarKind_Iso;
    case CalendarId::Buddhist:
      return capi::ICU4XAnyCalendarKind_Buddhist;
    case CalendarId::Chinese:
      return capi::ICU4XAnyCalendarKind_Chinese;
    case CalendarId::Coptic:
      return capi::ICU4XAnyCalendarKind_Coptic;
    case CalendarId::Dangi:
      return capi::ICU4XAnyCalendarKind_Dangi;
    case CalendarId::Ethiopian:
      return capi::ICU4XAnyCalendarKind_Ethiopian;
    case CalendarId::EthiopianAmeteAlem:
      return capi::ICU4XAnyCalendarKind_EthiopianAmeteAlem;
    case CalendarId::Gregorian:
      return capi::ICU4XAnyCalendarKind_Gregorian;
    case CalendarId::Hebrew:
      return capi::ICU4XAnyCalendarKind_Hebrew;
    case CalendarId::Indian:
      return capi::ICU4XAnyCalendarKind_Indian;
    case CalendarId::IslamicCivil:
      return capi::ICU4XAnyCalendarKind_IslamicCivil;
    case CalendarId::Islamic:
      return capi::ICU4XAnyCalendarKind_IslamicObservational;
    case CalendarId::IslamicRGSA:
      // ICU4X doesn't support a separate islamic-rgsa calendar, so we use the
      // observational calendar instead. This also matches ICU4C.
      return capi::ICU4XAnyCalendarKind_IslamicObservational;
    case CalendarId::IslamicTabular:
      return capi::ICU4XAnyCalendarKind_IslamicTabular;
    case CalendarId::IslamicUmmAlQura:
      return capi::ICU4XAnyCalendarKind_IslamicUmmAlQura;
    case CalendarId::Japanese:
      return capi::ICU4XAnyCalendarKind_Japanese;
    case CalendarId::Persian:
      return capi::ICU4XAnyCalendarKind_Persian;
    case CalendarId::ROC:
      return capi::ICU4XAnyCalendarKind_Roc;
  }
  MOZ_CRASH("invalid calendar id");
}

class ICU4XCalendarDeleter {
 public:
  void operator()(capi::ICU4XCalendar* ptr) {
    capi::ICU4XCalendar_destroy(ptr);
  }
};

using UniqueICU4XCalendar =
    mozilla::UniquePtr<capi::ICU4XCalendar, ICU4XCalendarDeleter>;

static UniqueICU4XCalendar CreateICU4XCalendar(JSContext* cx, CalendarId id) {
  auto result = capi::ICU4XCalendar_create_for_kind(
      mozilla::intl::GetDataProvider(), ToAnyCalendarKind(id));
  if (!result.is_ok) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INTERNAL_ERROR);
    return nullptr;
  }
  return UniqueICU4XCalendar{result.ok};
}

static uint32_t MaximumISOYear(CalendarId calendarId) {
  switch (calendarId) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem:
    case CalendarId::Gregorian:
    case CalendarId::Hebrew:
    case CalendarId::Indian:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicTabular:
    case CalendarId::Japanese:
    case CalendarId::Persian:
    case CalendarId::ROC: {
      // Passing values near INT32_{MIN,MAX} triggers ICU4X assertions, so we
      // have to handle large input years early.
      return 300'000;
    }

    case CalendarId::Chinese:
    case CalendarId::Dangi: {
      // Lower limit for these calendars to avoid running into ICU4X assertions.
      //
      // https://github.com/unicode-org/icu4x/issues/4917
      return 10'000;
    }

    case CalendarId::Islamic:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicUmmAlQura: {
      // Lower limit for these calendars to avoid running into ICU4X assertions.
      //
      // https://github.com/unicode-org/icu4x/issues/4917
      return 5'000;
    }
  }
  MOZ_CRASH("invalid calendar");
}

static uint32_t MaximumCalendarYear(CalendarId calendarId) {
  switch (calendarId) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem:
    case CalendarId::Gregorian:
    case CalendarId::Hebrew:
    case CalendarId::Indian:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicTabular:
    case CalendarId::Japanese:
    case CalendarId::Persian:
    case CalendarId::ROC: {
      // Passing values near INT32_{MIN,MAX} triggers ICU4X assertions, so we
      // have to handle large input years early.
      return 300'000;
    }

    case CalendarId::Chinese:
    case CalendarId::Dangi: {
      // Lower limit for these calendars to avoid running into ICU4X assertions.
      //
      // https://github.com/unicode-org/icu4x/issues/4917
      return 10'000;
    }

    case CalendarId::Islamic:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicUmmAlQura: {
      // Lower limit for these calendars to avoid running into ICU4X assertions.
      //
      // https://github.com/unicode-org/icu4x/issues/4917
      return 5'000;
    }
  }
  MOZ_CRASH("invalid calendar");
}

static void ReportCalendarFieldOverflow(JSContext* cx, const char* name,
                                        double num) {
  ToCStringBuf numCbuf;
  const char* numStr = NumberToCString(&numCbuf, num);

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_TEMPORAL_CALENDAR_OVERFLOW_FIELD, name,
                            numStr);
}

class ICU4XDateDeleter {
 public:
  void operator()(capi::ICU4XDate* ptr) { capi::ICU4XDate_destroy(ptr); }
};

using UniqueICU4XDate = mozilla::UniquePtr<capi::ICU4XDate, ICU4XDateDeleter>;

static UniqueICU4XDate CreateICU4XDate(JSContext* cx, const ISODate& date,
                                       CalendarId calendarId,
                                       const capi::ICU4XCalendar* calendar) {
  if (mozilla::Abs(date.year) > MaximumISOYear(calendarId)) {
    ReportCalendarFieldOverflow(cx, "year", date.year);
    return nullptr;
  }

  auto result = capi::ICU4XDate_create_from_iso_in_calendar(
      date.year, date.month, date.day, calendar);
  if (!result.is_ok) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INTERNAL_ERROR);
    return nullptr;
  }
  return UniqueICU4XDate{result.ok};
}

class ICU4XIsoDateDeleter {
 public:
  void operator()(capi::ICU4XIsoDate* ptr) { capi::ICU4XIsoDate_destroy(ptr); }
};

using UniqueICU4XIsoDate =
    mozilla::UniquePtr<capi::ICU4XIsoDate, ICU4XIsoDateDeleter>;

class ICU4XWeekCalculatorDeleter {
 public:
  void operator()(capi::ICU4XWeekCalculator* ptr) {
    capi::ICU4XWeekCalculator_destroy(ptr);
  }
};

using UniqueICU4XWeekCalculator =
    mozilla::UniquePtr<capi::ICU4XWeekCalculator, ICU4XWeekCalculatorDeleter>;

static UniqueICU4XWeekCalculator CreateICU4WeekCalculator(JSContext* cx,
                                                          CalendarId calendar) {
  MOZ_ASSERT(calendar == CalendarId::Gregorian);

  auto firstWeekday = capi::ICU4XIsoWeekday_Monday;
  uint8_t minWeekDays = 1;

  auto* result =
      capi::ICU4XWeekCalculator_create_from_first_day_of_week_and_min_week_days(
          firstWeekday, minWeekDays);
  return UniqueICU4XWeekCalculator{result};
}

// Define IMPLEMENTS_DR2126 if DR2126 is implemented.
//
// https://cplusplus.github.io/CWG/issues/2126.html
#if defined(__clang__)
#  if (__clang_major__ >= 12)
#    define IMPLEMENTS_DR2126
#  endif
#else
#  define IMPLEMENTS_DR2126
#endif

#ifdef IMPLEMENTS_DR2126
static constexpr size_t EraNameMaxLength() {
  size_t length = 0;
  for (auto calendar : AvailableCalendars()) {
    for (auto era : CalendarEras(calendar)) {
      for (auto name : CalendarEraNames(calendar, era)) {
        length = std::max(length, name.length());
      }
    }
  }
  return length;
}
#endif

static mozilla::Maybe<EraCode> EraForString(CalendarId calendar,
                                            JSLinearString* string) {
  MOZ_ASSERT(CalendarEraRelevant(calendar));

  // Note: Assigning MaxLength to EraNameMaxLength() breaks the CDT indexer.
  constexpr size_t MaxLength = 24;
#ifdef IMPLEMENTS_DR2126
  static_assert(MaxLength >= EraNameMaxLength(),
                "Storage size is at least as large as the largest known era");
#endif

  if (string->length() > MaxLength || !StringIsAscii(string)) {
    return mozilla::Nothing();
  }

  char chars[MaxLength] = {};
  CopyChars(reinterpret_cast<JS::Latin1Char*>(chars), *string);

  auto stringView = std::string_view{chars, string->length()};

  for (auto era : CalendarEras(calendar)) {
    for (auto name : CalendarEraNames(calendar, era)) {
      if (name == stringView) {
        return mozilla::Some(era);
      }
    }
  }
  return mozilla::Nothing();
}

static constexpr std::string_view IcuEraName(CalendarId calendar, EraCode era) {
  switch (calendar) {
    // https://docs.rs/icu/latest/icu/calendar/iso/struct.Iso.html#era-codes
    case CalendarId::ISO8601: {
      MOZ_ASSERT(era == EraCode::Standard);
      return "default";
    }

    // https://docs.rs/icu/latest/icu/calendar/buddhist/struct.Buddhist.html#era-codes
    case CalendarId::Buddhist: {
      MOZ_ASSERT(era == EraCode::Standard);
      return "be";
    }

    // https://docs.rs/icu/latest/icu/calendar/chinese/struct.Chinese.html#year-and-era-codes
    case CalendarId::Chinese: {
      MOZ_ASSERT(era == EraCode::Standard);
      return "chinese";
    }

    // https://docs.rs/icu/latest/icu/calendar/coptic/struct.Coptic.html#era-codes
    case CalendarId::Coptic: {
      MOZ_ASSERT(era == EraCode::Standard || era == EraCode::Inverse);
      return era == EraCode::Standard ? "ad" : "bd";
    }

    // https://docs.rs/icu/latest/icu/calendar/dangi/struct.Dangi.html#era-codes
    case CalendarId::Dangi: {
      MOZ_ASSERT(era == EraCode::Standard);
      return "dangi";
    }

    // https://docs.rs/icu/latest/icu/calendar/ethiopian/struct.Ethiopian.html#era-codes
    case CalendarId::Ethiopian: {
      MOZ_ASSERT(era == EraCode::Standard || era == EraCode::Inverse);
      return era == EraCode::Standard ? "incar" : "pre-incar";
    }

    // https://docs.rs/icu/latest/icu/calendar/ethiopian/struct.Ethiopian.html#era-codes
    case CalendarId::EthiopianAmeteAlem: {
      MOZ_ASSERT(era == EraCode::Standard);
      return "mundi";
    }

    // https://docs.rs/icu/latest/icu/calendar/gregorian/struct.Gregorian.html#era-codes
    case CalendarId::Gregorian: {
      MOZ_ASSERT(era == EraCode::Standard || era == EraCode::Inverse);
      return era == EraCode::Standard ? "ce" : "bce";
    }

    // https://docs.rs/icu/latest/icu/calendar/hebrew/struct.Hebrew.html
    case CalendarId::Hebrew: {
      MOZ_ASSERT(era == EraCode::Standard);
      return "am";
    }

    // https://docs.rs/icu/latest/icu/calendar/indian/struct.Indian.html#era-codes
    case CalendarId::Indian: {
      MOZ_ASSERT(era == EraCode::Standard);
      return "saka";
    }

    // https://docs.rs/icu/latest/icu/calendar/islamic/struct.IslamicCivil.html#era-codes
    // https://docs.rs/icu/latest/icu/calendar/islamic/struct.IslamicObservational.html#era-codes
    // https://docs.rs/icu/latest/icu/calendar/islamic/struct.IslamicTabular.html#era-codes
    // https://docs.rs/icu/latest/icu/calendar/islamic/struct.IslamicUmmAlQura.html#era-codes
    // https://docs.rs/icu/latest/icu/calendar/persian/struct.Persian.html#era-codes
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
    case CalendarId::Persian: {
      MOZ_ASSERT(era == EraCode::Standard);
      return "ah";
    }

    // https://docs.rs/icu/latest/icu/calendar/japanese/struct.Japanese.html#era-codes
    case CalendarId::Japanese: {
      switch (era) {
        case EraCode::Standard:
          return "ce";
        case EraCode::Inverse:
          return "bce";
        case EraCode::Meiji:
          return "meiji";
        case EraCode::Taisho:
          return "taisho";
        case EraCode::Showa:
          return "showa";
        case EraCode::Heisei:
          return "heisei";
        case EraCode::Reiwa:
          return "reiwa";
      }
      break;
    }

    // https://docs.rs/icu/latest/icu/calendar/roc/struct.Roc.html#era-codes
    case CalendarId::ROC: {
      MOZ_ASSERT(era == EraCode::Standard || era == EraCode::Inverse);
      return era == EraCode::Standard ? "roc" : "roc-inverse";
    }
  }
  JS_CONSTEXPR_CRASH("invalid era");
}

enum class CalendarError {
  // Catch-all kind for all other error types.
  Generic,

  // https://docs.rs/icu/latest/icu/calendar/enum.Error.html#variant.Overflow
  Overflow,

  // https://docs.rs/icu/latest/icu/calendar/enum.Error.html#variant.Underflow
  Underflow,

  // https://docs.rs/icu/latest/icu/calendar/enum.Error.html#variant.OutOfRange
  OutOfRange,

  // https://docs.rs/icu/latest/icu/calendar/enum.Error.html#variant.UnknownEra
  UnknownEra,

  // https://docs.rs/icu/latest/icu/calendar/enum.Error.html#variant.UnknownMonthCode
  UnknownMonthCode,
};

#ifdef DEBUG
static auto CalendarErasAsEnumSet(CalendarId calendarId) {
  // `mozilla::EnumSet<EraCode>(CalendarEras(calendarId))` doesn't work in old
  // GCC versions, so add all era codes manually to the enum set.
  mozilla::EnumSet<EraCode> eras{};
  for (auto era : CalendarEras(calendarId)) {
    eras += era;
  }
  return eras;
}
#endif

static mozilla::Result<UniqueICU4XDate, CalendarError> CreateDateFromCodes(
    CalendarId calendarId, const capi::ICU4XCalendar* calendar, EraYear eraYear,
    MonthCode monthCode, int32_t day) {
  MOZ_ASSERT(calendarId != CalendarId::ISO8601);
  MOZ_ASSERT(capi::ICU4XCalendar_kind(calendar) ==
             ToAnyCalendarKind(calendarId));
  MOZ_ASSERT(CalendarErasAsEnumSet(calendarId).contains(eraYear.era));
  MOZ_ASSERT_IF(CalendarEraRelevant(calendarId), eraYear.year > 0);
  MOZ_ASSERT(mozilla::Abs(eraYear.year) <= MaximumCalendarYear(calendarId));
  MOZ_ASSERT(CalendarMonthCodes(calendarId).contains(monthCode));
  MOZ_ASSERT(day > 0);
  MOZ_ASSERT(day <= CalendarDaysInMonth(calendarId).second);

  auto era = IcuEraName(calendarId, eraYear.era);
  auto monthCodeView = std::string_view{monthCode};
  auto date = capi::ICU4XDate_create_from_codes_in_calendar(
      era.data(), era.length(), eraYear.year, monthCodeView.data(),
      monthCodeView.length(), day, calendar);
  if (date.is_ok) {
    return UniqueICU4XDate{date.ok};
  }

  // Map possible calendar errors.
  //
  // Calendar error codes which can't happen for `create_from_codes_in_calendar`
  // are mapped to `CalendarError::Generic`.
  switch (date.err) {
    case capi::ICU4XError_CalendarOverflowError:
      return mozilla::Err(CalendarError::Overflow);
    case capi::ICU4XError_CalendarUnderflowError:
      return mozilla::Err(CalendarError::Underflow);
    case capi::ICU4XError_CalendarOutOfRangeError:
      return mozilla::Err(CalendarError::OutOfRange);
    case capi::ICU4XError_CalendarUnknownEraError:
      return mozilla::Err(CalendarError::UnknownEra);
    case capi::ICU4XError_CalendarUnknownMonthCodeError:
      return mozilla::Err(CalendarError::UnknownMonthCode);
    default:
      return mozilla::Err(CalendarError::Generic);
  }
}

/**
 * Return the first year (gannen) of a Japanese era.
 */
static bool FirstYearOfJapaneseEra(JSContext* cx, CalendarId calendarId,
                                   const capi::ICU4XCalendar* calendar,
                                   EraCode era, int32_t* result) {
  MOZ_ASSERT(calendarId == CalendarId::Japanese);
  MOZ_ASSERT(!CalendarEraStartsAtYearBoundary(calendarId, era));

  // All supported Japanese eras last at least one year, so December 31 is
  // guaranteed to be in the first year of the era.
  auto dateResult =
      CreateDateFromCodes(calendarId, calendar, {era, 1}, MonthCode{12}, 31);
  if (dateResult.isErr()) {
    MOZ_ASSERT(dateResult.inspectErr() == CalendarError::Generic,
               "unexpected non-generic calendar error");

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INTERNAL_ERROR);
    return false;
  }

  auto date = dateResult.unwrap();
  UniqueICU4XIsoDate isoDate{capi::ICU4XDate_to_iso(date.get())};

  int32_t isoYear = capi::ICU4XIsoDate_year(isoDate.get());
  MOZ_ASSERT(isoYear > 0, "unexpected era start before 1 CE");

  *result = isoYear;
  return true;
}

/**
 * Return the equivalent common era year for a Japanese era year.
 */
static bool JapaneseEraYearToCommonEraYear(JSContext* cx, CalendarId calendarId,
                                           const capi::ICU4XCalendar* calendar,
                                           EraYear eraYear, EraYear* result) {
  int32_t firstYearOfEra;
  if (!FirstYearOfJapaneseEra(cx, calendarId, calendar, eraYear.era,
                              &firstYearOfEra)) {
    return false;
  }

  // Map non-positive era years to years before the first era year:
  //
  //  1 Reiwa =  2019 CE
  //  0 Reiwa -> 2018 CE
  // -1 Reiwa -> 2017 CE
  // etc.
  //
  // Map too large era years to the next era:
  //
  // Heisei 31 =  2019 CE
  // Heisei 32 -> 2020 CE
  // ...

  int32_t year = (firstYearOfEra - 1) + eraYear.year;
  if (year > 0) {
    *result = {EraCode::Standard, year};
    return true;
  }
  *result = {EraCode::Inverse, int32_t(mozilla::Abs(year) + 1)};
  return true;
}

static UniqueICU4XDate CreateDateFromCodes(JSContext* cx, CalendarId calendarId,
                                           const capi::ICU4XCalendar* calendar,
                                           EraYear eraYear, MonthCode monthCode,
                                           int32_t day,
                                           TemporalOverflow overflow) {
  MOZ_ASSERT(CalendarMonthCodes(calendarId).contains(monthCode));
  MOZ_ASSERT(day > 0);
  MOZ_ASSERT(day <= CalendarDaysInMonth(calendarId).second);

  // Constrain day to the maximum possible day for the input month.
  //
  // Special cases like February 29 in leap years of the Gregorian calendar are
  // handled below.
  int32_t daysInMonth = CalendarDaysInMonth(calendarId, monthCode).second;
  if (overflow == TemporalOverflow::Constrain) {
    day = std::min(day, daysInMonth);
  } else {
    MOZ_ASSERT(overflow == TemporalOverflow::Reject);

    if (day > daysInMonth) {
      ReportCalendarFieldOverflow(cx, "day", day);
      return nullptr;
    }
  }

  // ICU4X doesn't support large dates, so we have to handle this case early.
  if (mozilla::Abs(eraYear.year) > MaximumCalendarYear(calendarId)) {
    ReportCalendarFieldOverflow(cx, "year", eraYear.year);
    return nullptr;
  }

  auto result =
      CreateDateFromCodes(calendarId, calendar, eraYear, monthCode, day);
  if (result.isOk()) {
    return result.unwrap();
  }

  switch (result.inspectErr()) {
    case CalendarError::UnknownMonthCode: {
      // We've asserted above that |monthCode| is valid for this calendar, so
      // any unknown month code must be for a leap month which doesn't happen in
      // the current year.
      MOZ_ASSERT(CalendarHasLeapMonths(calendarId));
      MOZ_ASSERT(monthCode.isLeapMonth());

      if (overflow == TemporalOverflow::Reject) {
        // Ensure the month code is null-terminated.
        char code[5] = {};
        auto monthCodeView = std::string_view{monthCode};
        monthCodeView.copy(code, monthCodeView.length());

        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_TEMPORAL_CALENDAR_INVALID_MONTHCODE,
                                 code);
        return nullptr;
      }

      // Retry as non-leap month when we're allowed to constrain.
      //
      // CalendarDateToISO ( calendar, fields, overflow )
      //
      // If the month is a leap month that doesn't exist in the year, pick
      // another date according to the cultural conventions of that calendar's
      // users. Usually this will result in the same day in the month before or
      // after where that month would normally fall in a leap year.
      //
      // Hebrew calendar:
      // Replace Adar I (M05L) with Adar (M06).
      //
      // Chinese/Dangi calendar:
      // Pick the next month, for example M03L -> M04, except for M12L, because
      // we don't want to switch over to the next year.

      // TODO: Temporal spec polyfill replaces M03L with M03 for Chinese/Dangi.
      // No idea what are the "cultural conventions" for these two calendars...
      //
      // https://github.com/tc39/proposal-intl-era-monthcode/issues/32

      int32_t nonLeapMonth = std::min(monthCode.ordinal() + 1, 12);
      auto nonLeapMonthCode = MonthCode{nonLeapMonth};
      return CreateDateFromCodes(cx, calendarId, calendar, eraYear,
                                 nonLeapMonthCode, day, overflow);
    }

    case CalendarError::Overflow: {
      // ICU4X throws an overflow error when:
      // 1. month > monthsInYear(year), or
      // 2. days > daysInMonthOf(year, month).
      //
      // Case 1 can't happen for month-codes, so it doesn't apply here.
      // Case 2 can only happen when |day| is larger than the minimum number
      // of days in the month.
      MOZ_ASSERT(day > CalendarDaysInMonth(calendarId, monthCode).first);

      if (overflow == TemporalOverflow::Reject) {
        ReportCalendarFieldOverflow(cx, "day", day);
        return nullptr;
      }

      auto firstDayOfMonth = CreateDateFromCodes(
          cx, calendarId, calendar, eraYear, monthCode, 1, overflow);
      if (!firstDayOfMonth) {
        return nullptr;
      }

      int32_t daysInMonth = DaysInMonth(firstDayOfMonth.get());
      MOZ_ASSERT(day > daysInMonth);
      return CreateDateFromCodes(cx, calendarId, calendar, eraYear, monthCode,
                                 daysInMonth, overflow);
    }

    case CalendarError::OutOfRange: {
      // ICU4X throws an out-of-range error if:
      // 1. Non-positive era years are given.
      // 2. Dates are before/after the requested named Japanese era.
      //
      // Case 1 doesn't happen for us, because we always pass strictly positive
      // era years, so this error must be for case 2.
      MOZ_ASSERT(calendarId == CalendarId::Japanese);
      MOZ_ASSERT(!CalendarEraStartsAtYearBoundary(calendarId, eraYear.era));

      EraYear commonEraYear;
      if (!JapaneseEraYearToCommonEraYear(cx, calendarId, calendar, eraYear,
                                          &commonEraYear)) {
        return nullptr;
      }
      return CreateDateFromCodes(cx, calendarId, calendar, commonEraYear,
                                 monthCode, day, overflow);
    }

    case CalendarError::Underflow:
    case CalendarError::UnknownEra:
      MOZ_ASSERT(false, "unexpected calendar error");
      break;

    case CalendarError::Generic:
      break;
  }

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_TEMPORAL_CALENDAR_INTERNAL_ERROR);
  return nullptr;
}

static UniqueICU4XDate CreateDateFrom(JSContext* cx, CalendarId calendarId,
                                      const capi::ICU4XCalendar* calendar,
                                      EraYear eraYear, int32_t month,
                                      int32_t day, TemporalOverflow overflow) {
  MOZ_ASSERT(calendarId != CalendarId::ISO8601);
  MOZ_ASSERT(month > 0);
  MOZ_ASSERT(day > 0);
  MOZ_ASSERT(month <= CalendarMonthsPerYear(calendarId));
  MOZ_ASSERT(day <= CalendarDaysInMonth(calendarId).second);

  switch (calendarId) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem:
    case CalendarId::Gregorian:
    case CalendarId::Indian:
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
    case CalendarId::Japanese:
    case CalendarId::Persian:
    case CalendarId::ROC: {
      MOZ_ASSERT(!CalendarHasLeapMonths(calendarId));

      // Use the month-code corresponding to the ordinal month number for
      // calendar systems without leap months.
      auto date = CreateDateFromCodes(cx, calendarId, calendar, eraYear,
                                      MonthCode{month}, day, overflow);
      if (!date) {
        return nullptr;
      }
      MOZ_ASSERT_IF(CalendarEraStartsAtYearBoundary(calendarId),
                    OrdinalMonth(calendarId, date.get()) == month);
      return date;
    }

    case CalendarId::Dangi:
    case CalendarId::Chinese: {
      static_assert(CalendarHasLeapMonths(CalendarId::Chinese));
      static_assert(CalendarMonthsPerYear(CalendarId::Chinese) == 13);
      static_assert(CalendarHasLeapMonths(CalendarId::Dangi));
      static_assert(CalendarMonthsPerYear(CalendarId::Dangi) == 13);

      MOZ_ASSERT(1 <= month && month <= 13);

      // Create date with month number replaced by month-code.
      auto monthCode = MonthCode{std::min(month, 12)};
      auto date = CreateDateFromCodes(cx, calendarId, calendar, eraYear,
                                      monthCode, day, overflow);
      if (!date) {
        return nullptr;
      }

      // If the ordinal month of |date| matches the input month, no additional
      // changes are necessary and we can directly return |date|.
      int32_t ordinal = OrdinalMonth(calendarId, date.get());
      if (ordinal == month) {
        return date;
      }

      // Otherwise we need to handle three cases:
      // 1. The input year contains a leap month and we need to adjust the
      //    month-code.
      // 2. The thirteenth month of a year without leap months was requested.
      // 3. The thirteenth month of a year with leap months was requested.
      if (ordinal > month) {
        MOZ_ASSERT(1 < month && month <= 12);

        // This case can only happen in leap years.
        MOZ_ASSERT(MonthsInYear(date.get()) == 13);

        // Leap months can occur after any month in the Chinese calendar.
        //
        // Example when the fourth month is a leap month between M03 and M04.
        //
        // Month code:     M01  M02  M03  M03L  M04  M05  M06 ...
        // Ordinal month:  1    2    3    4     5    6    7

        // The month can be off by exactly one.
        MOZ_ASSERT((ordinal - month) == 1);

        // First try the case when the previous month isn't a leap month. This
        // case can only occur when |month > 2|, because otherwise we know that
        // "M01L" is the correct answer.
        if (month > 2) {
          auto previousMonthCode = MonthCode{month - 1};
          date = CreateDateFromCodes(cx, calendarId, calendar, eraYear,
                                     previousMonthCode, day, overflow);
          if (!date) {
            return nullptr;
          }

          int32_t ordinal = OrdinalMonth(calendarId, date.get());
          if (ordinal == month) {
            return date;
          }
        }

        // Fall-through when the previous month is a leap month.
      } else {
        MOZ_ASSERT(month == 13);
        MOZ_ASSERT(ordinal == 12);

        // Years with leap months contain thirteen months.
        if (MonthsInYear(date.get()) != 13) {
          if (overflow == TemporalOverflow::Reject) {
            ReportCalendarFieldOverflow(cx, "month", month);
            return nullptr;
          }
          return date;
        }

        // Fall-through to return leap month "M12L" at the end of the year.
      }

      // Finally handle the case when the previous month is a leap month.
      auto leapMonthCode = MonthCode{month - 1, /* isLeapMonth= */ true};
      date = CreateDateFromCodes(cx, calendarId, calendar, eraYear,
                                 leapMonthCode, day, overflow);
      if (!date) {
        return nullptr;
      }
      MOZ_ASSERT(OrdinalMonth(calendarId, date.get()) == month,
                 "unexpected ordinal month");
      return date;
    }

    case CalendarId::Hebrew: {
      static_assert(CalendarHasLeapMonths(CalendarId::Hebrew));
      static_assert(CalendarMonthsPerYear(CalendarId::Hebrew) == 13);

      MOZ_ASSERT(1 <= month && month <= 13);

      // Create date with month number replaced by month-code.
      auto monthCode = MonthCode{std::min(month, 12)};
      auto date = CreateDateFromCodes(cx, calendarId, calendar, eraYear,
                                      monthCode, day, overflow);
      if (!date) {
        return nullptr;
      }

      // If the ordinal month of |date| matches the input month, no additional
      // changes are necessary and we can directly return |date|.
      int32_t ordinal = OrdinalMonth(calendarId, date.get());
      if (ordinal == month) {
        return date;
      }

      // Otherwise we need to handle two cases:
      // 1. The input year contains a leap month and we need to adjust the
      //    month-code.
      // 2. The thirteenth month of a year without leap months was requested.
      if (ordinal > month) {
        MOZ_ASSERT(1 < month && month <= 12);

        // This case can only happen in leap years.
        MOZ_ASSERT(MonthsInYear(date.get()) == 13);

        // Leap months can occur between M05 and M06 in the Hebrew calendar.
        //
        // Month code:     M01  M02  M03  M04  M05  M05L  M06 ...
        // Ordinal month:  1    2    3    4    5    6     7

        // The month can be off by exactly one.
        MOZ_ASSERT((ordinal - month) == 1);
      } else {
        MOZ_ASSERT(month == 13);
        MOZ_ASSERT(ordinal == 12);

        if (overflow == TemporalOverflow::Reject) {
          ReportCalendarFieldOverflow(cx, "month", month);
          return nullptr;
        }
        return date;
      }

      // The previous month is the leap month Adar I iff |month| is six.
      bool isLeapMonth = month == 6;
      auto previousMonthCode = MonthCode{month - 1, isLeapMonth};
      date = CreateDateFromCodes(cx, calendarId, calendar, eraYear,
                                 previousMonthCode, day, overflow);
      if (!date) {
        return nullptr;
      }
      MOZ_ASSERT(OrdinalMonth(calendarId, date.get()) == month,
                 "unexpected ordinal month");
      return date;
    }
  }
  MOZ_CRASH("invalid calendar id");
}

#ifdef IMPLEMENTS_DR2126
static constexpr size_t ICUEraNameMaxLength() {
  size_t length = 0;
  for (auto calendar : AvailableCalendars()) {
    for (auto era : CalendarEras(calendar)) {
      auto name = IcuEraName(calendar, era);
      length = std::max(length, name.length());
    }
  }
  return length;
}
#endif

/**
 * Retrieve the era code from |date| and then map the returned ICU4X era code to
 * the corresponding |EraCode| member.
 */
static bool CalendarDateEra(JSContext* cx, CalendarId calendar,
                            const capi::ICU4XDate* date, EraCode* result) {
  MOZ_ASSERT(calendar != CalendarId::ISO8601);

  // Note: Assigning MaxLength to ICUEraNameMaxLength() breaks the CDT indexer.
  constexpr size_t MaxLength = 15;
#ifdef IMPLEMENTS_DR2126

// Disable tautological-value-range-compare to avoid a bogus Clang warning.
// See bug 1956918 and bug 1936626.
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wtautological-value-range-compare"
#  endif

  static_assert(MaxLength >= ICUEraNameMaxLength(),
                "Storage size is at least as large as the largest known era");

#  ifdef __clang__
#    pragma clang diagnostic pop
#  endif

#endif

  // Storage for the largest known era string and the terminating NUL-character.
  char buf[MaxLength + 1] = {};
  auto writable = capi::diplomat_simple_writeable(buf, std::size(buf));

  if (!capi::ICU4XDate_era(date, &writable).is_ok) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INTERNAL_ERROR);
    return false;
  }
  MOZ_ASSERT(writable.buf == buf, "unexpected buffer relocation");

  auto dateEra = std::string_view{writable.buf, writable.len};

  // Map to era name to era code.
  for (auto era : CalendarEras(calendar)) {
    if (IcuEraName(calendar, era) == dateEra) {
      *result = era;
      return true;
    }
  }

  // Invalid/Unknown era name.
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_TEMPORAL_CALENDAR_INTERNAL_ERROR);
  return false;
}

/**
 * Return the extended (non-era) year from |date|.
 */
static bool CalendarDateYear(JSContext* cx, CalendarId calendar,
                             const capi::ICU4XDate* date, int32_t* result) {
  MOZ_ASSERT(calendar != CalendarId::ISO8601);

  // FIXME: ICU4X doesn't yet support CalendarDateYear, so we need to manually
  // adjust the era year to determine the non-era year.
  //
  // https://github.com/unicode-org/icu4x/issues/3962

  if (!CalendarEraRelevant(calendar)) {
    int32_t year = capi::ICU4XDate_year_in_era(date);
    *result = year;
    return true;
  }

  if (calendar != CalendarId::Japanese) {
    MOZ_ASSERT(CalendarEras(calendar).size() == 2);

    int32_t year = capi::ICU4XDate_year_in_era(date);
    MOZ_ASSERT(year > 0, "era years are strictly positive in ICU4X");

    EraCode era;
    if (!CalendarDateEra(cx, calendar, date, &era)) {
      return false;
    }

    // Map from era year to extended year.
    //
    // For example in the Gregorian calendar:
    //
    // ----------------------------
    // | Era Year | Extended Year |
    // | 2 CE     |  2            |
    // | 1 CE     |  1            |
    // | 1 BCE    |  0            |
    // | 2 BCE    | -1            |
    // ----------------------------
    if (era == EraCode::Inverse) {
      year = -(year - 1);
    } else {
      MOZ_ASSERT(era == EraCode::Standard);
    }

    *result = year;
    return true;
  }

  // Japanese uses a proleptic Gregorian calendar, so we can use the ISO year.
  UniqueICU4XIsoDate isoDate{capi::ICU4XDate_to_iso(date)};
  int32_t isoYear = capi::ICU4XIsoDate_year(isoDate.get());

  *result = isoYear;
  return true;
}

/**
 * Retrieve the month code from |date| and then map the returned ICU4X month
 * code to the corresponding |MonthCode| member.
 */
static bool CalendarDateMonthCode(JSContext* cx, CalendarId calendar,
                                  const capi::ICU4XDate* date,
                                  MonthCode* result) {
  MOZ_ASSERT(calendar != CalendarId::ISO8601);

  // Valid month codes are "M01".."M13" and "M01L".."M12L".
  constexpr size_t MaxLength =
      std::string_view{MonthCode::maxLeapMonth()}.length();
  static_assert(
      MaxLength > std::string_view{MonthCode::maxNonLeapMonth()}.length(),
      "string representation of max-leap month is larger");

  // Storage for the largest valid month code and the terminating NUL-character.
  char buf[MaxLength + 1] = {};
  auto writable = capi::diplomat_simple_writeable(buf, std::size(buf));

  if (!capi::ICU4XDate_month_code(date, &writable).is_ok) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INTERNAL_ERROR);
    return false;
  }
  MOZ_ASSERT(writable.buf == buf, "unexpected buffer relocation");

  auto view = std::string_view{writable.buf, writable.len};

  MOZ_ASSERT(view.length() >= 3);
  MOZ_ASSERT(view[0] == 'M');
  MOZ_ASSERT(mozilla::IsAsciiDigit(view[1]));
  MOZ_ASSERT(mozilla::IsAsciiDigit(view[2]));
  MOZ_ASSERT_IF(view.length() > 3, view[3] == 'L');

  int32_t ordinal =
      AsciiDigitToNumber(view[1]) * 10 + AsciiDigitToNumber(view[2]);
  bool isLeapMonth = view.length() > 3;
  auto monthCode = MonthCode{ordinal, isLeapMonth};

  static constexpr auto IrregularAdarII =
      MonthCode{6, /* isLeapMonth = */ true};
  static constexpr auto RegularAdarII = MonthCode{6};

  // Handle the irregular month code "M06L" for Adar II in leap years.
  //
  // https://docs.rs/icu/latest/icu/calendar/hebrew/struct.Hebrew.html#month-codes
  if (calendar == CalendarId::Hebrew && monthCode == IrregularAdarII) {
    monthCode = RegularAdarII;
  }

  if (DayOfMonthCanBeZero(calendar)) {
    // If |dayOfMonth| is zero, interpret as last day of previous month.
    int32_t dayOfMonth = capi::ICU4XDate_day_of_month(date);
    if (dayOfMonth == 0) {
      MOZ_ASSERT(ordinal > 1 && !isLeapMonth);
      monthCode = MonthCode{ordinal - 1};
    }
  }

  // The month code must be valid for this calendar.
  MOZ_ASSERT(CalendarMonthCodes(calendar).contains(monthCode));

  *result = monthCode;
  return true;
}

class MonthCodeString {
  // Zero-terminated month code string.
  char str_[4 + 1];

 public:
  explicit MonthCodeString(MonthCodeField field) {
    str_[0] = 'M';
    str_[1] = char('0' + (field.ordinal() / 10));
    str_[2] = char('0' + (field.ordinal() % 10));
    str_[3] = field.isLeapMonth() ? 'L' : '\0';
    str_[4] = '\0';
  }

  const char* toCString() const { return str_; }
};

/**
 * CalendarResolveFields ( calendar, fields, type )
 */
static bool ISOCalendarResolveMonth(JSContext* cx,
                                    Handle<CalendarFields> fields,
                                    double* result) {
  double month = fields.month();
  MOZ_ASSERT_IF(fields.has(CalendarField::Month),
                IsInteger(month) && month > 0);

  // CalendarResolveFields, steps 1.e.
  if (!fields.has(CalendarField::MonthCode)) {
    MOZ_ASSERT(fields.has(CalendarField::Month));

    *result = month;
    return true;
  }

  auto monthCode = fields.monthCode();

  // CalendarResolveFields, steps 1.f-k.
  int32_t ordinal = monthCode.ordinal();
  if (ordinal < 1 || ordinal > 12 || monthCode.isLeapMonth()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_TEMPORAL_CALENDAR_INVALID_MONTHCODE,
                             MonthCodeString{monthCode}.toCString());
    return false;
  }

  // CalendarResolveFields, steps 1.l-m.
  if (fields.has(CalendarField::Month) && month != ordinal) {
    ToCStringBuf cbuf;
    const char* monthStr = NumberToCString(&cbuf, month);

    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE_MONTHCODE,
                             MonthCodeString{monthCode}.toCString(), monthStr);
    return false;
  }

  // CalendarResolveFields, steps 1.n.
  *result = ordinal;
  return true;
}

struct EraYears {
  // Year starting from the calendar epoch.
  mozilla::Maybe<EraYear> fromEpoch;

  // Year starting from a specific calendar era.
  mozilla::Maybe<EraYear> fromEra;
};

static bool CalendarEraYear(JSContext* cx, CalendarId calendarId,
                            EraYear eraYear, EraYear* result) {
  MOZ_ASSERT(CalendarEraRelevant(calendarId));
  MOZ_ASSERT(mozilla::Abs(eraYear.year) <= MaximumCalendarYear(calendarId));

  if (eraYear.year > 0) {
    *result = eraYear;
    return true;
  }

  switch (eraYear.era) {
    case EraCode::Standard: {
      // Map non-positive era years as follows:
      //
      //  0 CE -> 1 BCE
      // -1 CE -> 2 BCE
      // etc.
      *result = {EraCode::Inverse, int32_t(mozilla::Abs(eraYear.year) + 1)};
      return true;
    }

    case EraCode::Inverse: {
      // Map non-positive era years as follows:
      //
      //  0 BCE -> 1 CE
      // -1 BCE -> 2 CE
      // etc.
      *result = {EraCode::Standard, int32_t(mozilla::Abs(eraYear.year) + 1)};
      return true;
    }

    case EraCode::Meiji:
    case EraCode::Taisho:
    case EraCode::Showa:
    case EraCode::Heisei:
    case EraCode::Reiwa: {
      MOZ_ASSERT(calendarId == CalendarId::Japanese);

      auto cal = CreateICU4XCalendar(cx, calendarId);
      if (!cal) {
        return false;
      }
      return JapaneseEraYearToCommonEraYear(cx, calendarId, cal.get(), eraYear,
                                            result);
    }
  }
  MOZ_CRASH("invalid era id");
}

/**
 * CalendarResolveFields ( calendar, fields, type )
 * CalendarDateToISO ( calendar, fields, overflow )
 * CalendarMonthDayToISOReferenceDate ( calendar, fields, overflow )
 *
 * Extract `year` and `eraYear` from |fields| and perform some initial
 * validation to ensure the values are valid for the requested calendar.
 */
static bool CalendarFieldYear(JSContext* cx, CalendarId calendar,
                              Handle<CalendarFields> fields, EraYears* result) {
  MOZ_ASSERT(fields.has(CalendarField::Year) ||
             fields.has(CalendarField::EraYear));

  // |eraYear| is to be ignored when not relevant for |calendar| per
  // CalendarResolveFields.
  bool hasRelevantEra =
      fields.has(CalendarField::Era) && CalendarEraRelevant(calendar);
  MOZ_ASSERT_IF(fields.has(CalendarField::Era), CalendarEraRelevant(calendar));

  // Case 1: |year| field is present.
  mozilla::Maybe<EraYear> fromEpoch;
  if (fields.has(CalendarField::Year)) {
    double year = fields.year();
    MOZ_ASSERT(IsInteger(year));

    int32_t intYear;
    if (!mozilla::NumberEqualsInt32(year, &intYear) ||
        mozilla::Abs(intYear) > MaximumCalendarYear(calendar)) {
      ReportCalendarFieldOverflow(cx, "year", year);
      return false;
    }

    fromEpoch = mozilla::Some(CalendarEraYear(calendar, intYear));
  } else {
    MOZ_ASSERT(hasRelevantEra);
  }

  // Case 2: |era| and |eraYear| fields are present and relevant for |calendar|.
  mozilla::Maybe<EraYear> fromEra;
  if (hasRelevantEra) {
    MOZ_ASSERT(fields.has(CalendarField::Era));
    MOZ_ASSERT(fields.has(CalendarField::EraYear));

    auto era = fields.era();
    MOZ_ASSERT(era);

    double eraYear = fields.eraYear();
    MOZ_ASSERT(IsInteger(eraYear));

    auto* linearEra = era->ensureLinear(cx);
    if (!linearEra) {
      return false;
    }

    // Ensure the requested era is valid for |calendar|.
    auto eraCode = EraForString(calendar, linearEra);
    if (!eraCode) {
      if (auto code = QuoteString(cx, era)) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_TEMPORAL_CALENDAR_INVALID_ERA,
                                 code.get());
      }
      return false;
    }

    int32_t intEraYear;
    if (!mozilla::NumberEqualsInt32(eraYear, &intEraYear) ||
        mozilla::Abs(intEraYear) > MaximumCalendarYear(calendar)) {
      ReportCalendarFieldOverflow(cx, "eraYear", eraYear);
      return false;
    }

    EraYear eraAndYear;
    if (!CalendarEraYear(cx, calendar, {*eraCode, intEraYear}, &eraAndYear)) {
      return false;
    }
    fromEra = mozilla::Some(eraAndYear);
  }

  *result = {fromEpoch, fromEra};
  return true;
}

struct Month {
  // Month code.
  MonthCode code;

  // Ordinal month number.
  int32_t ordinal = 0;
};

/**
 * CalendarResolveFields ( calendar, fields, type )
 * CalendarDateToISO ( calendar, fields, overflow )
 * CalendarMonthDayToISOReferenceDate ( calendar, fields, overflow )
 *
 * Extract `month` and `monthCode` from |fields| and perform some initial
 * validation to ensure the values are valid for the requested calendar.
 */
static bool CalendarFieldMonth(JSContext* cx, CalendarId calendar,
                               Handle<CalendarFields> fields,
                               TemporalOverflow overflow, Month* result) {
  MOZ_ASSERT(fields.has(CalendarField::Month) ||
             fields.has(CalendarField::MonthCode));

  // Case 1: |month| field is present.
  int32_t intMonth = 0;
  if (fields.has(CalendarField::Month)) {
    double month = fields.month();
    MOZ_ASSERT(IsInteger(month) && month > 0);

    if (!mozilla::NumberEqualsInt32(month, &intMonth)) {
      intMonth = 0;
    }

    const int32_t monthsPerYear = CalendarMonthsPerYear(calendar);
    if (intMonth < 1 || intMonth > monthsPerYear) {
      if (overflow == TemporalOverflow::Reject) {
        ReportCalendarFieldOverflow(cx, "month", month);
        return false;
      }
      MOZ_ASSERT(overflow == TemporalOverflow::Constrain);

      intMonth = monthsPerYear;
    }

    MOZ_ASSERT(intMonth > 0);
  }

  // Case 2: |monthCode| field is present.
  MonthCode fromMonthCode;
  if (fields.has(CalendarField::MonthCode)) {
    auto monthCode = fields.monthCode();
    int32_t ordinal = monthCode.ordinal();
    bool isLeapMonth = monthCode.isLeapMonth();

    constexpr int32_t minMonth = MonthCode{1}.ordinal();
    constexpr int32_t maxNonLeapMonth = MonthCode::maxNonLeapMonth().ordinal();
    constexpr int32_t maxLeapMonth = MonthCode::maxLeapMonth().ordinal();

    // Minimum month number is 1. Maximum month is 12 (or 13 when the calendar
    // uses epagomenal months).
    const int32_t maxMonth = isLeapMonth ? maxLeapMonth : maxNonLeapMonth;
    if (minMonth <= ordinal && ordinal <= maxMonth) {
      fromMonthCode = MonthCode{ordinal, isLeapMonth};
    }

    // Ensure the month code is valid for this calendar.
    const auto& monthCodes = CalendarMonthCodes(calendar);
    if (!monthCodes.contains(fromMonthCode)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_TEMPORAL_CALENDAR_INVALID_MONTHCODE,
                               MonthCodeString{monthCode}.toCString());
      return false;
    }
  }

  *result = {fromMonthCode, intMonth};
  return true;
}

/**
 * CalendarResolveFields ( calendar, fields, type )
 * CalendarDateToISO ( calendar, fields, overflow )
 * CalendarMonthDayToISOReferenceDate ( calendar, fields, overflow )
 *
 * Extract `day` from |fields| and perform some initial validation to ensure the
 * value is valid for the requested calendar.
 */
static bool CalendarFieldDay(JSContext* cx, CalendarId calendar,
                             Handle<CalendarFields> fields,
                             TemporalOverflow overflow, int32_t* result) {
  MOZ_ASSERT(fields.has(CalendarField::Day));

  double day = fields.day();
  MOZ_ASSERT(IsInteger(day) && day > 0);

  int32_t intDay;
  if (!mozilla::NumberEqualsInt32(day, &intDay)) {
    intDay = 0;
  }

  // Constrain to a valid day value in this calendar.
  int32_t daysPerMonth = CalendarDaysInMonth(calendar).second;
  if (intDay < 1 || intDay > daysPerMonth) {
    if (overflow == TemporalOverflow::Reject) {
      ReportCalendarFieldOverflow(cx, "day", day);
      return false;
    }
    MOZ_ASSERT(overflow == TemporalOverflow::Constrain);

    intDay = daysPerMonth;
  }

  *result = intDay;
  return true;
}

/**
 * CalendarResolveFields ( calendar, fields, type )
 *
 * > The operation throws a TypeError exception if the properties of fields are
 * > internally inconsistent within the calendar [...]. For example:
 * >
 * > [...] The values for "era" and "eraYear" do not together identify the same
 * > year as the value for "year".
 */
static bool CalendarFieldEraYearMatchesYear(JSContext* cx, CalendarId calendar,
                                            Handle<CalendarFields> fields,
                                            const capi::ICU4XDate* date) {
  MOZ_ASSERT(fields.has(CalendarField::EraYear));
  MOZ_ASSERT(fields.has(CalendarField::Year));

  double year = fields.year();
  MOZ_ASSERT(IsInteger(year));

  int32_t intYear;
  MOZ_ALWAYS_TRUE(mozilla::NumberEqualsInt32(year, &intYear));

  int32_t yearFromEraYear;
  if (!CalendarDateYear(cx, calendar, date, &yearFromEraYear)) {
    return false;
  }

  // The user requested year must match the actual (extended/epoch) year.
  if (intYear != yearFromEraYear) {
    ToCStringBuf yearCbuf;
    const char* yearStr = NumberToCString(&yearCbuf, intYear);

    ToCStringBuf fromEraCbuf;
    const char* fromEraStr = NumberToCString(&fromEraCbuf, yearFromEraYear);

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE_YEAR,
                              yearStr, fromEraStr);
    return false;
  }
  return true;
}

/**
 * CalendarResolveFields ( calendar, fields, type )
 *
 * > The operation throws a TypeError exception if the properties of fields are
 * > internally inconsistent within the calendar [...]. For example:
 * >
 * > If "month" and "monthCode" in the calendar [...] do not identify the same
 * > month.
 */
static bool CalendarFieldMonthCodeMatchesMonth(JSContext* cx,
                                               CalendarId calendarId,
                                               Handle<CalendarFields> fields,
                                               const capi::ICU4XDate* date,
                                               int32_t month) {
  int32_t ordinal = OrdinalMonth(calendarId, date);

  // The user requested month must match the actual ordinal month.
  if (month != ordinal) {
    ToCStringBuf cbuf;
    const char* monthStr = NumberToCString(&cbuf, fields.month());

    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE_MONTHCODE,
                             MonthCodeString{fields.monthCode()}.toCString(),
                             monthStr);
    return false;
  }
  return true;
}

static ISODate ToISODate(const capi::ICU4XDate* date) {
  UniqueICU4XIsoDate isoDate{capi::ICU4XDate_to_iso(date)};

  int32_t isoYear = capi::ICU4XIsoDate_year(isoDate.get());

  int32_t isoMonth = capi::ICU4XIsoDate_month(isoDate.get());
  MOZ_ASSERT(1 <= isoMonth && isoMonth <= 12);

  int32_t isoDay = capi::ICU4XIsoDate_day_of_month(isoDate.get());

  // TODO: Workaround for <https://github.com/unicode-org/icu4x/issues/5070>.
  if (isoDay == 0) {
    MOZ_ASSERT(capi::ICU4XCalendar_kind(capi::ICU4XDate_calendar(date)) ==
               capi::ICU4XAnyCalendarKind_Indian);
    isoDay = 31;
    isoMonth = 12;
    isoYear -= 1;
  }

  MOZ_ASSERT(1 <= isoDay && isoDay <= ::ISODaysInMonth(isoYear, isoMonth));

  return {isoYear, isoMonth, isoDay};
}

static UniqueICU4XDate CreateDateFrom(JSContext* cx, CalendarId calendar,
                                      const capi::ICU4XCalendar* cal,
                                      const EraYears& eraYears,
                                      const Month& month, int32_t day,
                                      Handle<CalendarFields> fields,
                                      TemporalOverflow overflow) {
  // Use |eraYear| if present, so we can more easily check for consistent
  // |year| and |eraYear| fields.
  auto eraYear = eraYears.fromEra ? *eraYears.fromEra : *eraYears.fromEpoch;

  UniqueICU4XDate date;
  if (month.code != MonthCode{}) {
    date = CreateDateFromCodes(cx, calendar, cal, eraYear, month.code, day,
                               overflow);
  } else {
    date = CreateDateFrom(cx, calendar, cal, eraYear, month.ordinal, day,
                          overflow);
  }
  if (!date) {
    return nullptr;
  }

  // |year| and |eraYear| must be consistent.
  if (eraYears.fromEpoch && eraYears.fromEra) {
    if (!CalendarFieldEraYearMatchesYear(cx, calendar, fields, date.get())) {
      return nullptr;
    }
  }

  // |month| and |monthCode| must be consistent.
  if (month.code != MonthCode{} && month.ordinal > 0) {
    if (!CalendarFieldMonthCodeMatchesMonth(cx, calendar, fields, date.get(),
                                            month.ordinal)) {
      return nullptr;
    }
  }

  return date;
}

/**
 * RegulateISODate ( year, month, day, overflow )
 */
static bool RegulateISODate(JSContext* cx, int32_t year, double month,
                            double day, TemporalOverflow overflow,
                            ISODate* result) {
  MOZ_ASSERT(IsInteger(month));
  MOZ_ASSERT(IsInteger(day));

  // Step 1.
  if (overflow == TemporalOverflow::Constrain) {
    // Step 1.a.
    int32_t m = int32_t(std::clamp(month, 1.0, 12.0));

    // Step 1.b.
    double daysInMonth = double(::ISODaysInMonth(year, m));

    // Step 1.c.
    int32_t d = int32_t(std::clamp(day, 1.0, daysInMonth));

    // Step 3. (Inlined call to CreateISODateRecord.)
    *result = {year, m, d};
    return true;
  }

  // Step 2.a.
  MOZ_ASSERT(overflow == TemporalOverflow::Reject);

  // Step 2.b.
  if (!ThrowIfInvalidISODate(cx, year, month, day)) {
    return false;
  }

  // Step 3. (Inlined call to CreateISODateRecord.)
  *result = {year, int32_t(month), int32_t(day)};
  return true;
}

/**
 * CalendarDateToISO ( calendar, fields, overflow )
 */
static bool CalendarDateToISO(JSContext* cx, CalendarId calendar,
                              Handle<CalendarFields> fields,
                              TemporalOverflow overflow, ISODate* result) {
  // Step 1.
  if (calendar == CalendarId::ISO8601) {
    // Step 1.a.
    MOZ_ASSERT(fields.has(CalendarField::Year));
    MOZ_ASSERT(fields.has(CalendarField::Month) ||
               fields.has(CalendarField::MonthCode));
    MOZ_ASSERT(fields.has(CalendarField::Day));

    // Remaining steps from CalendarResolveFields to resolve the month.
    double month;
    if (!ISOCalendarResolveMonth(cx, fields, &month)) {
      return false;
    }

    int32_t intYear;
    if (!mozilla::NumberEqualsInt32(fields.year(), &intYear)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
      return false;
    }

    // Step 1.b.
    return RegulateISODate(cx, intYear, month, fields.day(), overflow, result);
  }

  // Step 2.

  EraYears eraYears;
  if (!CalendarFieldYear(cx, calendar, fields, &eraYears)) {
    return false;
  }

  Month month;
  if (!CalendarFieldMonth(cx, calendar, fields, overflow, &month)) {
    return false;
  }

  int32_t day;
  if (!CalendarFieldDay(cx, calendar, fields, overflow, &day)) {
    return false;
  }

  auto cal = CreateICU4XCalendar(cx, calendar);
  if (!cal) {
    return false;
  }

  auto date = CreateDateFrom(cx, calendar, cal.get(), eraYears, month, day,
                             fields, overflow);
  if (!date) {
    return false;
  }

  *result = ToISODate(date.get());
  return true;
}

/**
 * CalendarMonthDayToISOReferenceDate ( calendar, fields, overflow )
 */
static bool CalendarMonthDayToISOReferenceDate(JSContext* cx,
                                               CalendarId calendar,
                                               Handle<CalendarFields> fields,
                                               TemporalOverflow overflow,
                                               ISODate* result) {
  // Step 1.
  if (calendar == CalendarId::ISO8601) {
    // Step 1.a.
    MOZ_ASSERT(fields.has(CalendarField::Month) ||
               fields.has(CalendarField::MonthCode));
    MOZ_ASSERT(fields.has(CalendarField::Day));

    // Remaining steps from CalendarResolveFields to resolve the month.
    double month;
    if (!ISOCalendarResolveMonth(cx, fields, &month)) {
      return false;
    }

    // Step 1.b.
    int32_t referenceISOYear = 1972;

    // Step 1.c.
    double year =
        !fields.has(CalendarField::Year) ? referenceISOYear : fields.year();

    int32_t intYear;
    if (!mozilla::NumberEqualsInt32(year, &intYear)) {
      // Calendar cycles repeat every 400 years in the Gregorian calendar.
      intYear = int32_t(std::fmod(year, 400));
    }

    // Step 1.d.
    ISODate regulated;
    if (!RegulateISODate(cx, intYear, month, fields.day(), overflow,
                         &regulated)) {
      return false;
    }

    // Step 1.e.
    *result = {referenceISOYear, regulated.month, regulated.day};
    return true;
  }

  // Step 2.

  EraYears eraYears;
  if (fields.has(CalendarField::Year) || fields.has(CalendarField::EraYear)) {
    if (!CalendarFieldYear(cx, calendar, fields, &eraYears)) {
      return false;
    }
  } else {
    MOZ_ASSERT(fields.has(CalendarField::MonthCode));
  }

  Month month;
  if (!CalendarFieldMonth(cx, calendar, fields, overflow, &month)) {
    return false;
  }

  int32_t day;
  if (!CalendarFieldDay(cx, calendar, fields, overflow, &day)) {
    return false;
  }

  auto cal = CreateICU4XCalendar(cx, calendar);
  if (!cal) {
    return false;
  }

  // We first have to compute the month-code if it wasn't provided to us.
  auto monthCode = month.code;
  if (fields.has(CalendarField::Year) || fields.has(CalendarField::EraYear)) {
    auto date = CreateDateFrom(cx, calendar, cal.get(), eraYears, month, day,
                               fields, overflow);
    if (!date) {
      return false;
    }

    // This operation throws a RangeError if the ISO 8601 year corresponding to
    // `fields.[[Year]]` is outside the valid limits.
    auto isoDate = ToISODate(date.get());
    if (!ISODateWithinLimits(isoDate)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
      return false;
    }

    if (!fields.has(CalendarField::MonthCode)) {
      if (!CalendarDateMonthCode(cx, calendar, date.get(), &monthCode)) {
        return false;
      }
    }
    MOZ_ASSERT(monthCode != MonthCode{});

    if (overflow == TemporalOverflow::Constrain) {
      // Call into ICU4X if `day` exceeds the minimum number of days.
      int32_t minDaysInMonth = CalendarDaysInMonth(calendar, monthCode).first;
      if (day > minDaysInMonth) {
        day = DayOfMonth(calendar, date.get());
      }
    } else {
      MOZ_ASSERT(overflow == TemporalOverflow::Reject);
      MOZ_ASSERT(day == DayOfMonth(calendar, date.get()));
    }
  } else {
    MOZ_ASSERT(monthCode != MonthCode{});

    // Constrain `day` to maximum possible day of the input month.
    int32_t maxDaysInMonth = CalendarDaysInMonth(calendar, monthCode).second;
    if (overflow == TemporalOverflow::Constrain) {
      day = std::min(day, maxDaysInMonth);
    } else {
      MOZ_ASSERT(overflow == TemporalOverflow::Reject);

      if (day > maxDaysInMonth) {
        ReportCalendarFieldOverflow(cx, "day", day);
        return false;
      }
    }
  }

  // Try years starting from 31 December, 1972.
  constexpr auto isoReferenceDate = ISODate{1972, 12, 31};

  auto fromIsoDate = CreateICU4XDate(cx, isoReferenceDate, calendar, cal.get());
  if (!fromIsoDate) {
    return false;
  }

  // Find the calendar year for the ISO reference date.
  int32_t calendarYear;
  if (!CalendarDateYear(cx, calendar, fromIsoDate.get(), &calendarYear)) {
    return false;
  }

  // 10'000 is sufficient to find all possible month-days, even for rare cases
  // like `{calendar: "chinese", monthCode: "M09L", day: 30}`.
  constexpr size_t maxIterations = 10'000;

  UniqueICU4XDate date;
  for (size_t i = 0; i < maxIterations; i++) {
    // This loop can run for a long time.
    if (!CheckForInterrupt(cx)) {
      return false;
    }

    auto candidateYear = CalendarEraYear(calendar, calendarYear);

    auto result =
        CreateDateFromCodes(calendar, cal.get(), candidateYear, monthCode, day);
    if (result.isOk()) {
      // Make sure the resolved date is before December 31, 1972.
      auto isoDate = ToISODate(result.inspect().get());
      if (isoDate.year > isoReferenceDate.year) {
        calendarYear -= 1;
        continue;
      }

      date = result.unwrap();
      break;
    }

    switch (result.inspectErr()) {
      case CalendarError::UnknownMonthCode: {
        MOZ_ASSERT(CalendarHasLeapMonths(calendar));
        MOZ_ASSERT(monthCode.isLeapMonth());

        // Try the next candidate year if the requested leap month doesn't
        // occur in the current year.
        calendarYear -= 1;
        continue;
      }

      case CalendarError::Overflow: {
        // ICU4X throws an overflow error when:
        // 1. month > monthsInYear(year), or
        // 2. days > daysInMonthOf(year, month).
        //
        // Case 1 can't happen for month-codes, so it doesn't apply here.
        // Case 2 can only happen when |day| is larger than the minimum number
        // of days in the month.
        MOZ_ASSERT(day > CalendarDaysInMonth(calendar, monthCode).first);

        // Try next candidate year to find an earlier year which can fulfill
        // the input request.
        calendarYear -= 1;
        continue;
      }

      case CalendarError::OutOfRange:
      case CalendarError::Underflow:
      case CalendarError::UnknownEra:
        MOZ_ASSERT(false, "unexpected calendar error");
        break;

      case CalendarError::Generic:
        break;
    }

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INTERNAL_ERROR);
    return false;
  }

  // We shouldn't end up here with |maxIterations == 10'000|, but just in case
  // still handle this case and report an error.
  if (!date) {
    ReportCalendarFieldOverflow(cx, "day", day);
    return false;
  }

  // |month| and |monthCode| must be consistent.
  if (month.code != MonthCode{} && month.ordinal > 0) {
    if (!CalendarFieldMonthCodeMatchesMonth(cx, calendar, fields, date.get(),
                                            month.ordinal)) {
      return false;
    }
  }

  *result = ToISODate(date.get());
  return true;
}

enum class FieldType { Date, YearMonth, MonthDay };

/**
 * CalendarResolveFields ( calendar, fields, type )
 */
static bool CalendarResolveFields(JSContext* cx, CalendarId calendar,
                                  Handle<CalendarFields> fields,
                                  FieldType type) {
  // Step 1.
  if (calendar == CalendarId::ISO8601) {
    // Steps 1.a-e.
    const char* missingField = nullptr;
    if ((type == FieldType::Date || type == FieldType::YearMonth) &&
        !fields.has(CalendarField::Year)) {
      missingField = "year";
    } else if ((type == FieldType::Date || type == FieldType::MonthDay) &&
               !fields.has(CalendarField::Day)) {
      missingField = "day";
    } else if (!fields.has(CalendarField::MonthCode) &&
               !fields.has(CalendarField::Month)) {
      missingField = "month";
    }

    if (missingField) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_TEMPORAL_CALENDAR_MISSING_FIELD,
                                missingField);
      return false;
    }

    // Steps 1.f-n. (Handled in ISOCalendarResolveMonth.)

    return true;
  }

  // Step 2.

  // Date and Month-Day require |day| to be present.
  bool requireDay = type == FieldType::Date || type == FieldType::MonthDay;

  // Date and Year-Month require |year| (or |eraYear|) to be present.
  // Month-Day requires |year| (or |eraYear|) if |monthCode| is absent.
  bool requireYear = type == FieldType::Date || type == FieldType::YearMonth ||
                     !fields.has(CalendarField::MonthCode);

  // Determine if any calendar fields are missing.
  const char* missingField = nullptr;
  if (!fields.has(CalendarField::MonthCode) &&
      !fields.has(CalendarField::Month)) {
    // |monthCode| or |month| must be present.
    missingField = "monthCode";
  } else if (requireDay && !fields.has(CalendarField::Day)) {
    missingField = "day";
  } else if (!CalendarEraRelevant(calendar)) {
    if (requireYear && !fields.has(CalendarField::Year)) {
      missingField = "year";
    }
  } else {
    if (fields.has(CalendarField::Era) != fields.has(CalendarField::EraYear)) {
      // |era| and |eraYear| must either both be present or both absent.
      missingField = fields.has(CalendarField::Era) ? "eraYear" : "era";
    } else if (requireYear && !fields.has(CalendarField::EraYear) &&
               !fields.has(CalendarField::Year)) {
      missingField = "eraYear";
    }
  }

  if (missingField) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_MISSING_FIELD,
                              missingField);
    return false;
  }

  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[Era]] field.
 */
bool js::temporal::CalendarEra(JSContext* cx, Handle<CalendarValue> calendar,
                               const ISODate& date,
                               MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setUndefined();
    return true;
  }

  // Step 2.
  if (!CalendarEraRelevant(calendarId)) {
    result.setUndefined();
    return true;
  }

  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  EraCode era;
  if (!CalendarDateEra(cx, calendarId, dt.get(), &era)) {
    return false;
  }

  auto* str = NewStringCopy<CanGC>(cx, CalendarEraName(calendarId, era));
  if (!str) {
    return false;
  }

  result.setString(str);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[EraYear]] field.
 */
bool js::temporal::CalendarEraYear(JSContext* cx,
                                   Handle<CalendarValue> calendar,
                                   const ISODate& date,
                                   MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setUndefined();
    return true;
  }

  // Step 2.
  if (!CalendarEraRelevant(calendarId)) {
    result.setUndefined();
    return true;
  }

  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  int32_t year = capi::ICU4XDate_year_in_era(dt.get());
  result.setInt32(year);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[Year]] field.
 */
bool js::temporal::CalendarYear(JSContext* cx, Handle<CalendarValue> calendar,
                                const ISODate& date,
                                MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setInt32(date.year);
    return true;
  }

  // Step 2.
  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  int32_t year;
  if (!CalendarDateYear(cx, calendarId, dt.get(), &year)) {
    return false;
  }

  result.setInt32(year);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[Month]] field.
 */
bool js::temporal::CalendarMonth(JSContext* cx, Handle<CalendarValue> calendar,
                                 const ISODate& date,
                                 MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setInt32(date.month);
    return true;
  }

  // Step 2.
  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  int32_t month = OrdinalMonth(calendarId, dt.get());
  result.setInt32(month);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[MonthCode]] field.
 */
bool js::temporal::CalendarMonthCode(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     const ISODate& date,
                                     MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    // Steps 1.a-b.
    auto monthCode = MonthCode{date.month};
    JSString* str = NewStringCopy<CanGC>(cx, std::string_view{monthCode});
    if (!str) {
      return false;
    }

    result.setString(str);
    return true;
  }

  // Step 2.
  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  MonthCode monthCode;
  if (!CalendarDateMonthCode(cx, calendarId, dt.get(), &monthCode)) {
    return false;
  }

  auto* str = NewStringCopy<CanGC>(cx, std::string_view{monthCode});
  if (!str) {
    return false;
  }

  result.setString(str);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[Day]] field.
 */
bool js::temporal::CalendarDay(JSContext* cx, Handle<CalendarValue> calendar,
                               const ISODate& date,
                               MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setInt32(date.day);
    return true;
  }

  // Step 2.
  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  int32_t day = DayOfMonth(calendarId, dt.get());
  result.setInt32(day);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[DayOfWeek]] field.
 */
bool js::temporal::CalendarDayOfWeek(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     const ISODate& date,
                                     MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setInt32(ISODayOfWeek(date));
    return true;
  }

  // Step 2.
  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  // Week day codes are correctly ordered.
  static_assert(capi::ICU4XIsoWeekday_Monday == 1);
  static_assert(capi::ICU4XIsoWeekday_Tuesday == 2);
  static_assert(capi::ICU4XIsoWeekday_Wednesday == 3);
  static_assert(capi::ICU4XIsoWeekday_Thursday == 4);
  static_assert(capi::ICU4XIsoWeekday_Friday == 5);
  static_assert(capi::ICU4XIsoWeekday_Saturday == 6);
  static_assert(capi::ICU4XIsoWeekday_Sunday == 7);

  capi::ICU4XIsoWeekday day = capi::ICU4XDate_day_of_week(dt.get());
  result.setInt32(static_cast<int32_t>(day));
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[DayOfYear]] field.
 */
bool js::temporal::CalendarDayOfYear(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     const ISODate& date,
                                     MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setInt32(ISODayOfYear(date));
    return true;
  }

  // Step 2.
  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  // Workaround for https://github.com/unicode-org/icu4x/issues/5655
  if (calendarId == CalendarId::Japanese) {
    // Use the extended year instead of the era year to correctly handle the
    // case when the era changes in the current year. This can happen in the
    // Japanese calendar.
    int32_t year;
    if (!CalendarDateYear(cx, calendarId, dt.get(), &year)) {
      return false;
    }
    auto eraYear = CalendarEraYear(calendarId, year);

    int32_t dayOfYear = DayOfMonth(calendarId, dt.get());
    int32_t month = OrdinalMonth(calendarId, dt.get());

    // Add the number of days of all preceding months to compute the overall day
    // of the year.
    while (month > 1) {
      auto previousMonth = CreateDateFrom(cx, calendarId, cal.get(), eraYear,
                                          --month, 1, TemporalOverflow::Reject);
      if (!previousMonth) {
        return false;
      }

      dayOfYear += DaysInMonth(previousMonth.get());
    }

    MOZ_ASSERT(dayOfYear <= DaysInYear(dt.get()));

    result.setInt32(dayOfYear);
    return true;
  }

  int32_t day = DayOfYear(dt.get());
  result.setInt32(day);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[WeekOfYear]].[[Week]] field.
 */
bool js::temporal::CalendarWeekOfYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      const ISODate& date,
                                      MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setInt32(ISOWeekOfYear(date).week);
    return true;
  }

  // Step 2.

  // Non-Gregorian calendars don't get week-of-year support for now.
  //
  // https://github.com/tc39/proposal-intl-era-monthcode/issues/15
  if (calendarId != CalendarId::Gregorian) {
    result.setUndefined();
    return true;
  }

  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  auto weekCal = CreateICU4WeekCalculator(cx, calendarId);
  if (!weekCal) {
    return false;
  }

  auto week = capi::ICU4XDate_week_of_year(dt.get(), weekCal.get());
  if (!week.is_ok) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INTERNAL_ERROR);
    return false;
  }

  result.setInt32(week.ok.week);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[WeekOfYear]].[[Year]] field.
 */
bool js::temporal::CalendarYearOfWeek(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      const ISODate& date,
                                      MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setInt32(ISOWeekOfYear(date).year);
    return true;
  }

  // Step 2.

  // Non-Gregorian calendars don't get week-of-year support for now.
  //
  // https://github.com/tc39/proposal-intl-era-monthcode/issues/15
  if (calendarId != CalendarId::Gregorian) {
    result.setUndefined();
    return true;
  }

  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  auto weekCal = CreateICU4WeekCalculator(cx, calendarId);
  if (!weekCal) {
    return false;
  }

  auto week = capi::ICU4XDate_week_of_year(dt.get(), weekCal.get());
  if (!week.is_ok) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INTERNAL_ERROR);
    return false;
  }

  int32_t relative = 0;
  switch (week.ok.unit) {
    case capi::ICU4XWeekRelativeUnit_Previous:
      relative = -1;
      break;
    case capi::ICU4XWeekRelativeUnit_Current:
      relative = 0;
      break;
    case capi::ICU4XWeekRelativeUnit_Next:
      relative = 1;
      break;
  }

  int32_t calendarYear;
  if (!CalendarDateYear(cx, calendarId, dt.get(), &calendarYear)) {
    return false;
  }

  result.setInt32(calendarYear + relative);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[DaysInWeek]] field.
 */
bool js::temporal::CalendarDaysInWeek(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      const ISODate& date,
                                      MutableHandle<Value> result) {
  // All supported ICU4X calendars use a 7-day week and so does the ISO 8601
  // calendar.
  //
  // This function isn't supported through the ICU4X FFI, so we have to
  // hardcode the result.

  // Step 1-2.
  result.setInt32(7);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[DaysInMonth]] field.
 */
bool js::temporal::CalendarDaysInMonth(JSContext* cx,
                                       Handle<CalendarValue> calendar,
                                       const ISODate& date,
                                       MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setInt32(::ISODaysInMonth(date.year, date.month));
    return true;
  }

  // Step 2.
  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  int32_t days = DaysInMonth(dt.get());
  result.setInt32(days);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[DaysInYear]] field.
 */
bool js::temporal::CalendarDaysInYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      const ISODate& date,
                                      MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setInt32(ISODaysInYear(date.year));
    return true;
  }

  // Step 2.
  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  int32_t days = DaysInYear(dt.get());
  result.setInt32(days);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[MonthsInYear]] field.
 */
bool js::temporal::CalendarMonthsInYear(JSContext* cx,
                                        Handle<CalendarValue> calendar,
                                        const ISODate& date,
                                        MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setInt32(12);
    return true;
  }

  // Step 2
  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  int32_t months = MonthsInYear(dt.get());
  result.setInt32(months);
  return true;
}

/**
 * CalendarISOToDate ( calendar, isoDate )
 *
 * Return the Calendar Date Record's [[InLeapYear]] field.
 */
bool js::temporal::CalendarInLeapYear(JSContext* cx,
                                      Handle<CalendarValue> calendar,
                                      const ISODate& date,
                                      MutableHandle<Value> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    result.setBoolean(IsISOLeapYear(date.year));
    return true;
  }

  // Step 2.

  // FIXME: Not supported in ICU4X.
  //
  // https://github.com/unicode-org/icu4x/issues/5654

  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  bool inLeapYear = false;
  switch (calendarId) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Gregorian:
    case CalendarId::Japanese:
    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem:
    case CalendarId::Indian:
    case CalendarId::Persian:
    case CalendarId::ROC: {
      MOZ_ASSERT(!CalendarHasLeapMonths(calendarId));

      // Solar calendars have either 365 or 366 days per year.
      int32_t days = DaysInYear(dt.get());
      MOZ_ASSERT(days == 365 || days == 366);

      // Leap years have 366 days.
      inLeapYear = days == 366;
      break;
    }

    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura: {
      MOZ_ASSERT(!CalendarHasLeapMonths(calendarId));

      // Lunar Islamic calendars have either 354 or 355 days per year.
      //
      // Allow 353 days to workaround
      // <https://github.com/unicode-org/icu4x/issues/4930>.
      int32_t days = DaysInYear(dt.get());
      MOZ_ASSERT(days == 353 || days == 354 || days == 355);

      // Leap years have 355 days.
      inLeapYear = days == 355;
      break;
    }

    case CalendarId::Chinese:
    case CalendarId::Dangi:
    case CalendarId::Hebrew: {
      MOZ_ASSERT(CalendarHasLeapMonths(calendarId));

      // Calendars with separate leap months have either 12 or 13 months per
      // year.
      int32_t months = MonthsInYear(dt.get());
      MOZ_ASSERT(months == 12 || months == 13);

      // Leap years have 13 months.
      inLeapYear = months == 13;
      break;
    }
  }

  result.setBoolean(inLeapYear);
  return true;
}

enum class DateFieldType { Date, YearMonth, MonthDay };

/**
 * ISODateToFields ( calendar, isoDate, type )
 */
static bool ISODateToFields(JSContext* cx, Handle<CalendarValue> calendar,
                            const ISODate& date, DateFieldType type,
                            MutableHandle<CalendarFields> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  result.set(CalendarFields{});

  // Steps 2-6. (Optimization for the ISO 8601 calendar.)
  if (calendarId == CalendarId::ISO8601) {
    // Step 2. (Not applicable in our implementation.)

    // Step 3.
    result.setMonthCode(MonthCode{date.month});

    // Step 4.
    if (type == DateFieldType::MonthDay || type == DateFieldType::Date) {
      result.setDay(date.day);
    }

    // Step 5.
    if (type == DateFieldType::YearMonth || type == DateFieldType::Date) {
      result.setYear(date.year);
    }

    // Step 6.
    return true;
  }

  // Step 2.
  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, date, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  // Step 3.
  MonthCode monthCode;
  if (!CalendarDateMonthCode(cx, calendarId, dt.get(), &monthCode)) {
    return false;
  }
  result.setMonthCode(monthCode);

  // Step 4.
  if (type == DateFieldType::MonthDay || type == DateFieldType::Date) {
    int32_t day = DayOfMonth(calendarId, dt.get());
    result.setDay(day);
  }

  // Step 5.
  if (type == DateFieldType::YearMonth || type == DateFieldType::Date) {
    int32_t year;
    if (!CalendarDateYear(cx, calendarId, dt.get(), &year)) {
      return false;
    }
    result.setYear(year);
  }

  // Step 6.
  return true;
}

/**
 * ISODateToFields ( calendar, isoDate, type )
 */
bool js::temporal::ISODateToFields(JSContext* cx, Handle<PlainDate> date,
                                   MutableHandle<CalendarFields> result) {
  return ISODateToFields(cx, date.calendar(), date, DateFieldType::Date,
                         result);
}

/**
 * ISODateToFields ( calendar, isoDate, type )
 */
bool js::temporal::ISODateToFields(JSContext* cx,
                                   Handle<PlainDateTime> dateTime,
                                   MutableHandle<CalendarFields> result) {
  return ISODateToFields(cx, dateTime.calendar(), dateTime.date(),
                         DateFieldType::Date, result);
}

/**
 * ISODateToFields ( calendar, isoDate, type )
 */
bool js::temporal::ISODateToFields(JSContext* cx,
                                   Handle<PlainMonthDay> monthDay,
                                   MutableHandle<CalendarFields> result) {
  return ISODateToFields(cx, monthDay.calendar(), monthDay.date(),
                         DateFieldType::MonthDay, result);
}

/**
 * ISODateToFields ( calendar, isoDate, type )
 */
bool js::temporal::ISODateToFields(JSContext* cx,
                                   Handle<PlainYearMonth> yearMonth,
                                   MutableHandle<CalendarFields> result) {
  return ISODateToFields(cx, yearMonth.calendar(), yearMonth.date(),
                         DateFieldType::YearMonth, result);
}

/**
 * CalendarDateFromFields ( calendar, fields, overflow )
 */
bool js::temporal::CalendarDateFromFields(JSContext* cx,
                                          Handle<CalendarValue> calendar,
                                          Handle<CalendarFields> fields,
                                          TemporalOverflow overflow,
                                          MutableHandle<PlainDate> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (!CalendarResolveFields(cx, calendarId, fields, FieldType::Date)) {
    return false;
  }

  // Step 2.
  ISODate date;
  if (!CalendarDateToISO(cx, calendarId, fields, overflow, &date)) {
    return false;
  }

  // Steps 3-4.
  return CreateTemporalDate(cx, date, calendar, result);
}

/**
 * CalendarYearMonthFromFields ( calendar, fields, overflow )
 */
bool js::temporal::CalendarYearMonthFromFields(
    JSContext* cx, Handle<CalendarValue> calendar,
    Handle<CalendarFields> fields, TemporalOverflow overflow,
    MutableHandle<PlainYearMonth> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (!CalendarResolveFields(cx, calendarId, fields, FieldType::YearMonth)) {
    return false;
  }

  // Step 2.
  int32_t firstDayIndex = 1;

  // Step 3.
  Rooted<CalendarFields> resolvedFields(cx, CalendarFields{fields});
  resolvedFields.setDay(firstDayIndex);

  // Step 4.
  ISODate date;
  if (!CalendarDateToISO(cx, calendarId, resolvedFields, overflow, &date)) {
    return false;
  }

  // Steps 5-6.
  return CreateTemporalYearMonth(cx, date, calendar, result);
}

/**
 * CalendarMonthDayFromFields ( calendar, fields, overflow )
 */
bool js::temporal::CalendarMonthDayFromFields(
    JSContext* cx, Handle<CalendarValue> calendar,
    Handle<CalendarFields> fields, TemporalOverflow overflow,
    MutableHandle<PlainMonthDay> result) {
  auto calendarId = calendar.identifier();

  // Step 1.
  if (!CalendarResolveFields(cx, calendarId, fields, FieldType::MonthDay)) {
    return false;
  }

  // Step 2.
  ISODate date;
  if (!CalendarMonthDayToISOReferenceDate(cx, calendarId, fields, overflow,
                                          &date)) {
    return false;
  }

  // Step 3-4.
  return CreateTemporalMonthDay(cx, date, calendar, result);
}

/**
 * Mathematical Operations, "modulo" notation.
 */
static int32_t NonNegativeModulo(int64_t x, int32_t y) {
  MOZ_ASSERT(y > 0);

  int32_t result = mozilla::AssertedCast<int32_t>(x % y);
  return (result < 0) ? (result + y) : result;
}

/**
 * RegulateISODate ( year, month, day, overflow )
 *
 * With |overflow = "constrain"|.
 */
static ISODate ConstrainISODate(const ISODate& date) {
  const auto& [year, month, day] = date;

  // Step 1.a.
  int32_t m = std::clamp(month, 1, 12);

  // Step 1.b.
  int32_t daysInMonth = ::ISODaysInMonth(year, m);

  // Step 1.c.
  int32_t d = std::clamp(day, 1, daysInMonth);

  // Step 3.
  return {year, m, d};
}

/**
 * RegulateISODate ( year, month, day, overflow )
 */
static bool RegulateISODate(JSContext* cx, const ISODate& date,
                            TemporalOverflow overflow, ISODate* result) {
  // Step 1.
  if (overflow == TemporalOverflow::Constrain) {
    // Steps 1.a-c and 3.
    *result = ConstrainISODate(date);
    return true;
  }

  // Step 2.a.
  MOZ_ASSERT(overflow == TemporalOverflow::Reject);

  // Step 2.b.
  if (!ThrowIfInvalidISODate(cx, date)) {
    return false;
  }

  // Step 3. (Inlined call to CreateISODateRecord.)
  *result = date;
  return true;
}

struct BalancedYearMonth final {
  int64_t year = 0;
  int32_t month = 0;
};

/**
 * BalanceISOYearMonth ( year, month )
 */
static BalancedYearMonth BalanceISOYearMonth(int64_t year, int64_t month) {
  MOZ_ASSERT(std::abs(year) < (int64_t(1) << 33),
             "year is the addition of plain-date year with duration years");
  MOZ_ASSERT(std::abs(month) < (int64_t(1) << 33),
             "month is the addition of plain-date month with duration months");

  // Step 1. (Not applicable in our implementation.)

  // Step 2.
  int64_t balancedYear = year + temporal::FloorDiv(month - 1, 12);

  // Step 3.
  int32_t balancedMonth = NonNegativeModulo(month - 1, 12) + 1;
  MOZ_ASSERT(1 <= balancedMonth && balancedMonth <= 12);

  // Step 4.
  return {balancedYear, balancedMonth};
}

static BalancedYearMonth BalanceYearMonth(int64_t year, int64_t month,
                                          int32_t monthsPerYear) {
  MOZ_ASSERT(std::abs(year) < (int64_t(1) << 33),
             "year is the addition of plain-date year with duration years");
  MOZ_ASSERT(std::abs(month) < (int64_t(1) << 33),
             "month is the addition of plain-date month with duration months");

  int64_t balancedYear = year + temporal::FloorDiv(month - 1, monthsPerYear);

  int32_t balancedMonth = NonNegativeModulo(month - 1, monthsPerYear) + 1;
  MOZ_ASSERT(1 <= balancedMonth && balancedMonth <= monthsPerYear);

  return {balancedYear, balancedMonth};
}

/**
 * CalendarDateAdd ( calendar, isoDate, duration, overflow )
 */
static bool AddISODate(JSContext* cx, const ISODate& isoDate,
                       const DateDuration& duration, TemporalOverflow overflow,
                       ISODate* result) {
  MOZ_ASSERT(ISODateWithinLimits(isoDate));
  MOZ_ASSERT(IsValidDuration(duration));

  // Step 1.a.
  auto yearMonth = BalanceISOYearMonth(isoDate.year + duration.years,
                                       isoDate.month + duration.months);
  MOZ_ASSERT(1 <= yearMonth.month && yearMonth.month <= 12);

  auto balancedYear = mozilla::CheckedInt<int32_t>(yearMonth.year);
  if (!balancedYear.isValid()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  // Step 1.b.
  ISODate regulated;
  if (!RegulateISODate(cx, {balancedYear.value(), yearMonth.month, isoDate.day},
                       overflow, &regulated)) {
    return false;
  }
  if (!ISODateWithinLimits(regulated)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  // Step 1.c.
  int64_t days = duration.days + duration.weeks * 7;

  // Step 1.d.
  ISODate balanced;
  if (!BalanceISODate(cx, regulated, days, &balanced)) {
    return false;
  }
  MOZ_ASSERT(IsValidISODate(balanced));

  *result = balanced;
  return true;
}

struct CalendarDate {
  int32_t year = 0;
  MonthCode monthCode = {};
  int32_t day = 0;
};

/**
 * CompareISODate adjusted for calendar dates.
 */
static int32_t CompareCalendarDate(const CalendarDate& one,
                                   const CalendarDate& two) {
  if (one.year != two.year) {
    return one.year < two.year ? -1 : 1;
  }
  if (one.monthCode != two.monthCode) {
    return one.monthCode < two.monthCode ? -1 : 1;
  }
  if (one.day != two.day) {
    return one.day < two.day ? -1 : 1;
  }
  return 0;
}

static bool ToCalendarDate(JSContext* cx, CalendarId calendarId,
                           const capi::ICU4XDate* dt, CalendarDate* result) {
  int32_t year;
  if (!CalendarDateYear(cx, calendarId, dt, &year)) {
    return false;
  }

  MonthCode monthCode;
  if (!CalendarDateMonthCode(cx, calendarId, dt, &monthCode)) {
    return false;
  }

  int32_t day = DayOfMonth(calendarId, dt);

  *result = {year, monthCode, day};
  return true;
}

/**
 * Store a calendar date in a |ISODate| struct when leap months don't matter.
 */
static bool ToCalendarDate(JSContext* cx, CalendarId calendarId,
                           const capi::ICU4XDate* dt, ISODate* result) {
  MOZ_ASSERT(!CalendarHasLeapMonths(calendarId));

  int32_t year;
  if (!CalendarDateYear(cx, calendarId, dt, &year)) {
    return false;
  }

  int32_t month = OrdinalMonth(calendarId, dt);
  int32_t day = DayOfMonth(calendarId, dt);

  *result = {year, month, day};
  return true;
}

static bool AddYearMonthDuration(JSContext* cx, CalendarId calendarId,
                                 const ISODate& calendarDate,
                                 const DateDuration& duration,
                                 CalendarDate* result) {
  MOZ_ASSERT(!CalendarHasLeapMonths(calendarId));
  MOZ_ASSERT(IsValidDuration(duration));

  auto [year, month, day] = calendarDate;

  // Months per year are fixed, so we can directly compute the final number of
  // years.
  auto yearMonth =
      BalanceYearMonth(year + duration.years, month + duration.months,
                       CalendarMonthsPerYear(calendarId));

  auto balancedYear = mozilla::CheckedInt<int32_t>(yearMonth.year);
  if (!balancedYear.isValid()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  *result = {balancedYear.value(), MonthCode{yearMonth.month}, day};
  return true;
}

static bool AddYearMonthDuration(JSContext* cx, CalendarId calendarId,
                                 const capi::ICU4XCalendar* calendar,
                                 const CalendarDate& calendarDate,
                                 const DateDuration& duration,
                                 CalendarDate* result) {
  MOZ_ASSERT(CalendarHasLeapMonths(calendarId));
  MOZ_ASSERT(IsValidDuration(duration));

  auto [year, monthCode, day] = calendarDate;

  // Add all duration years.
  auto durationYear = mozilla::CheckedInt<int32_t>(year) + duration.years;
  if (!durationYear.isValid()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }
  year = durationYear.value();

  // Months per year are variable, so we have construct a new date for each
  // year to balance the years and months.
  int64_t months = duration.months;
  if (months != 0) {
    auto eraYear = CalendarEraYear(calendarId, year);
    auto firstDayOfMonth =
        CreateDateFromCodes(cx, calendarId, calendar, eraYear, monthCode, 1,
                            TemporalOverflow::Constrain);
    if (!firstDayOfMonth) {
      return false;
    }

    if (months > 0) {
      while (true) {
        // Check if adding |months| is still in the current year.
        int32_t month = OrdinalMonth(calendarId, firstDayOfMonth.get());
        int32_t monthsInYear = MonthsInYear(firstDayOfMonth.get());
        if (month + months <= monthsInYear) {
          break;
        }

        // We've crossed a year boundary. Increase |year| and adjust |months|.
        year += 1;
        months -= (monthsInYear - month + 1);

        // Restart the loop with the first month of the next year.
        eraYear = CalendarEraYear(calendarId, year);
        firstDayOfMonth = CreateDateFrom(cx, calendarId, calendar, eraYear, 1,
                                         1, TemporalOverflow::Constrain);
        if (!firstDayOfMonth) {
          return false;
        }
      }
    } else {
      int32_t monthsPerYear = CalendarMonthsPerYear(calendarId);

      while (true) {
        // Check if subtracting |months| is still in the current year.
        int32_t month = OrdinalMonth(calendarId, firstDayOfMonth.get());
        if (month + months >= 1) {
          break;
        }

        // We've crossed a year boundary. Decrease |year| and adjust |months|.
        year -= 1;
        months += month;

        // Restart the loop with the last month of the previous year.
        eraYear = CalendarEraYear(calendarId, year);
        firstDayOfMonth =
            CreateDateFrom(cx, calendarId, calendar, eraYear, monthsPerYear, 1,
                           TemporalOverflow::Constrain);
        if (!firstDayOfMonth) {
          return false;
        }
      }
    }

    // Compute the actual month to find the correct month code.
    int32_t month = OrdinalMonth(calendarId, firstDayOfMonth.get()) + months;
    firstDayOfMonth = CreateDateFrom(cx, calendarId, calendar, eraYear, month,
                                     1, TemporalOverflow::Constrain);
    if (!firstDayOfMonth) {
      return false;
    }

    if (!CalendarDateMonthCode(cx, calendarId, firstDayOfMonth.get(),
                               &monthCode)) {
      return false;
    }
  }

  *result = {year, monthCode, day};
  return true;
}

static bool AddNonISODate(JSContext* cx, CalendarId calendarId,
                          const ISODate& isoDate, const DateDuration& duration,
                          TemporalOverflow overflow, ISODate* result) {
  MOZ_ASSERT(ISODateWithinLimits(isoDate));
  MOZ_ASSERT(IsValidDuration(duration));

  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dt = CreateICU4XDate(cx, isoDate, calendarId, cal.get());
  if (!dt) {
    return false;
  }

  CalendarDate calendarDate;
  if (!CalendarHasLeapMonths(calendarId)) {
    ISODate date;
    if (!ToCalendarDate(cx, calendarId, dt.get(), &date)) {
      return false;
    }
    if (!AddYearMonthDuration(cx, calendarId, date, duration, &calendarDate)) {
      return false;
    }
  } else {
    CalendarDate date;
    if (!ToCalendarDate(cx, calendarId, dt.get(), &date)) {
      return false;
    }
    if (!AddYearMonthDuration(cx, calendarId, cal.get(), date, duration,
                              &calendarDate)) {
      return false;
    }
  }

  // Regulate according to |overflow|.
  auto eraYear = CalendarEraYear(calendarId, calendarDate.year);
  auto regulated =
      CreateDateFromCodes(cx, calendarId, cal.get(), eraYear,
                          calendarDate.monthCode, calendarDate.day, overflow);
  if (!regulated) {
    return false;
  }

  // Compute the corresponding ISO date.
  auto regulatedIso = ToISODate(regulated.get());
  if (!ISODateWithinLimits(regulatedIso)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  // Add duration days and weeks.
  int64_t days = duration.days + duration.weeks * 7;

  // Adding days isn't calendar-specific, so we can use BalanceISODate.
  ISODate balancedIso;
  if (!BalanceISODate(cx, regulatedIso, days, &balancedIso)) {
    return false;
  }
  MOZ_ASSERT(IsValidISODate(balancedIso));

  *result = balancedIso;
  return true;
}

static bool AddCalendarDate(JSContext* cx, CalendarId calendarId,
                            const ISODate& isoDate,
                            const DateDuration& duration,
                            TemporalOverflow overflow, ISODate* result) {
  // ICU4X doesn't yet provide a public API for CalendarDateAdd.
  //
  // https://github.com/unicode-org/icu4x/issues/3964

  // If neither |years| nor |months| are present, just delegate to the ISO 8601
  // calendar version. This works because all supported calendars use a 7-days
  // week.
  if (duration.years == 0 && duration.months == 0) {
    return AddISODate(cx, isoDate, duration, overflow, result);
  }

  switch (calendarId) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Gregorian:
    case CalendarId::Japanese:
    case CalendarId::ROC:
      // Use the ISO 8601 calendar if the calendar system starts its year at the
      // same time as the ISO 8601 calendar and all months exactly match the
      // ISO 8601 calendar months.
      return AddISODate(cx, isoDate, duration, overflow, result);

    case CalendarId::Chinese:
    case CalendarId::Coptic:
    case CalendarId::Dangi:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem:
    case CalendarId::Hebrew:
    case CalendarId::Indian:
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
    case CalendarId::Persian:
      return AddNonISODate(cx, calendarId, isoDate, duration, overflow, result);
  }
  MOZ_CRASH("invalid calendar id");
}

/**
 * CalendarDateAdd ( calendar, isoDate, duration, overflow )
 */
bool js::temporal::CalendarDateAdd(JSContext* cx,
                                   Handle<CalendarValue> calendar,
                                   const ISODate& isoDate,
                                   const DateDuration& duration,
                                   TemporalOverflow overflow, ISODate* result) {
  MOZ_ASSERT(ISODateWithinLimits(isoDate));
  MOZ_ASSERT(IsValidDuration(duration));

  auto calendarId = calendar.identifier();

  // Steps 1-2.
  if (calendarId == CalendarId::ISO8601) {
    if (!AddISODate(cx, isoDate, duration, overflow, result)) {
      return false;
    }
  } else {
    if (!AddCalendarDate(cx, calendarId, isoDate, duration, overflow, result)) {
      return false;
    }
  }

  // Step 3.
  if (!ISODateWithinLimits(*result)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PLAIN_DATE_INVALID);
    return false;
  }

  // Step 4.
  return true;
}

/**
 * CalendarDateUntil ( calendar, one, two, largestUnit )
 */
static DateDuration DifferenceISODate(const ISODate& one, const ISODate& two,
                                      TemporalUnit largestUnit) {
  MOZ_ASSERT(IsValidISODate(one));
  MOZ_ASSERT(IsValidISODate(two));

  // Both inputs are also within the date limits.
  MOZ_ASSERT(ISODateWithinLimits(one));
  MOZ_ASSERT(ISODateWithinLimits(two));

  MOZ_ASSERT(TemporalUnit::Year <= largestUnit &&
             largestUnit <= TemporalUnit::Day);

  // Step 1.a.
  int32_t sign = -CompareISODate(one, two);

  // Step 1.b.
  if (sign == 0) {
    return {};
  }

  // Step 1.c.
  int32_t years = 0;

  // Step 1.e. (Reordered)
  int32_t months = 0;

  // Steps 1.d and 1.f.
  if (largestUnit == TemporalUnit::Year || largestUnit == TemporalUnit::Month) {
    years = two.year - one.year;
    months = two.month - one.month;

    auto intermediate = ISODate{one.year + years, one.month, one.day};
    if (CompareISODate(intermediate, two) * sign > 0) {
      years -= sign;
      months += 12 * sign;
    }

    intermediate = ISODate{one.year + years, one.month + months, one.day};
    if (intermediate.month > 12) {
      intermediate.month -= 12;
      intermediate.year += 1;
    } else if (intermediate.month < 1) {
      intermediate.month += 12;
      intermediate.year -= 1;
    }
    if (CompareISODate(intermediate, two) * sign > 0) {
      months -= sign;
    }

    if (largestUnit == TemporalUnit::Month) {
      months += years * 12;
      years = 0;
    }
  }

  // Step 1.g.
  auto intermediate = BalanceISOYearMonth(one.year + years, one.month + months);

  // Step 1.h.
  auto constrained = ConstrainISODate(
      ISODate{int32_t(intermediate.year), intermediate.month, one.day});

  // Step 1.i.
  int64_t weeks = 0;

  // Steps 1.k-n.
  int64_t days = MakeDay(two) - MakeDay(constrained);

  // Step 1.j. (Reordered)
  if (largestUnit == TemporalUnit::Week) {
    weeks = days / 7;
    days %= 7;
  }

  // Step 1.o.
  auto result = DateDuration{
      int64_t(years),
      int64_t(months),
      int64_t(weeks),
      int64_t(days),
  };
  MOZ_ASSERT(IsValidDuration(result));
  return result;
}

static bool DifferenceNonISODate(JSContext* cx, CalendarId calendarId,
                                 const ISODate& one, const ISODate& two,
                                 TemporalUnit largestUnit,
                                 DateDuration* result) {
  // Both inputs are also within the date limits.
  MOZ_ASSERT(ISODateWithinLimits(one));
  MOZ_ASSERT(ISODateWithinLimits(two));

  MOZ_ASSERT(TemporalUnit::Year <= largestUnit &&
             largestUnit <= TemporalUnit::Month);

  if (one == two) {
    *result = {};
    return true;
  }

  auto cal = CreateICU4XCalendar(cx, calendarId);
  if (!cal) {
    return false;
  }

  auto dtOne = CreateICU4XDate(cx, one, calendarId, cal.get());
  if (!dtOne) {
    return false;
  }

  auto dtTwo = CreateICU4XDate(cx, two, calendarId, cal.get());
  if (!dtTwo) {
    return false;
  }

  int32_t years = 0;
  int32_t months = 0;

  ISODate constrainedIso;
  if (!CalendarHasLeapMonths(calendarId)) {
    // If the months per year are fixed, we can use a modified DifferenceISODate
    // implementation to compute the date duration.
    int32_t monthsPerYear = CalendarMonthsPerYear(calendarId);

    ISODate oneDate;
    if (!ToCalendarDate(cx, calendarId, dtOne.get(), &oneDate)) {
      return false;
    }

    ISODate twoDate;
    if (!ToCalendarDate(cx, calendarId, dtTwo.get(), &twoDate)) {
      return false;
    }

    int32_t sign = -CompareISODate(oneDate, twoDate);
    MOZ_ASSERT(sign != 0);

    years = twoDate.year - oneDate.year;
    months = twoDate.month - oneDate.month;

    // If |oneDate + years| surpasses |twoDate|, reduce |years| by one and add
    // |monthsPerYear| to |months|. The next step will balance the intermediate
    // result.
    auto intermediate =
        ISODate{oneDate.year + years, oneDate.month, oneDate.day};
    if (CompareISODate(intermediate, twoDate) * sign > 0) {
      years -= sign;
      months += monthsPerYear * sign;
    }

    // Add both |years| and |months| and then balance the intermediate result to
    // ensure its month is within the valid bounds.
    intermediate =
        ISODate{oneDate.year + years, oneDate.month + months, oneDate.day};
    if (intermediate.month > monthsPerYear) {
      intermediate.month -= monthsPerYear;
      intermediate.year += 1;
    } else if (intermediate.month < 1) {
      intermediate.month += monthsPerYear;
      intermediate.year -= 1;
    }

    // If |intermediate| surpasses |twoDate|, reduce |month| by one.
    if (CompareISODate(intermediate, twoDate) * sign > 0) {
      months -= sign;
    }

    // Convert years to months if necessary.
    if (largestUnit == TemporalUnit::Month) {
      months += years * monthsPerYear;
      years = 0;
    }

    // Constrain to a proper date.
    auto balanced = BalanceYearMonth(oneDate.year + years,
                                     oneDate.month + months, monthsPerYear);

    auto eraYear = CalendarEraYear(calendarId, balanced.year);
    auto constrained =
        CreateDateFrom(cx, calendarId, cal.get(), eraYear, balanced.month,
                       oneDate.day, TemporalOverflow::Constrain);
    if (!constrained) {
      return false;
    }
    constrainedIso = ToISODate(constrained.get());

    MOZ_ASSERT(CompareISODate(constrainedIso, two) * sign <= 0,
               "constrained doesn't surpass two");
  } else {
    CalendarDate oneDate;
    if (!ToCalendarDate(cx, calendarId, dtOne.get(), &oneDate)) {
      return false;
    }

    CalendarDate twoDate;
    if (!ToCalendarDate(cx, calendarId, dtTwo.get(), &twoDate)) {
      return false;
    }

    int32_t sign = -CompareCalendarDate(oneDate, twoDate);
    MOZ_ASSERT(sign != 0);

    years = twoDate.year - oneDate.year;

    // If |oneDate + years| surpasses |twoDate|, reduce |years| by one and add
    // |monthsPerYear| to |months|. The next step will balance the intermediate
    // result.
    auto eraYear = CalendarEraYear(calendarId, oneDate.year + years);
    auto constrained = CreateDateFromCodes(cx, calendarId, cal.get(), eraYear,
                                           oneDate.monthCode, oneDate.day,
                                           TemporalOverflow::Constrain);
    if (!constrained) {
      return false;
    }

    CalendarDate constrainedDate;
    if (!ToCalendarDate(cx, calendarId, constrained.get(), &constrainedDate)) {
      return false;
    }

    if (CompareCalendarDate(constrainedDate, twoDate) * sign > 0) {
      years -= sign;
    }

    // Add as many months as possible without surpassing |twoDate|.
    while (true) {
      CalendarDate intermediateDate;
      if (!AddYearMonthDuration(cx, calendarId, cal.get(), oneDate,
                                {years, months + sign}, &intermediateDate)) {
        return false;
      }
      if (CompareCalendarDate(intermediateDate, twoDate) * sign > 0) {
        break;
      }
      months += sign;
      constrainedDate = intermediateDate;
    }
    MOZ_ASSERT(std::abs(months) < CalendarMonthsPerYear(calendarId));

    // Convert years to months if necessary.
    if (largestUnit == TemporalUnit::Month && years != 0) {
      auto monthsUntilEndOfYear = [calendarId](const capi::ICU4XDate* date) {
        int32_t month = OrdinalMonth(calendarId, date);
        int32_t monthsInYear = MonthsInYear(date);
        MOZ_ASSERT(1 <= month && month <= monthsInYear);

        return monthsInYear - month + 1;
      };

      auto monthsSinceStartOfYear = [calendarId](const capi::ICU4XDate* date) {
        return OrdinalMonth(calendarId, date) - 1;
      };

      // Add months until end of year resp. since start of year.
      if (sign > 0) {
        months += monthsUntilEndOfYear(dtOne.get());
      } else {
        months -= monthsSinceStartOfYear(dtOne.get());
      }

      // Months in full year.
      for (int32_t y = sign; y != years; y += sign) {
        auto eraYear = CalendarEraYear(calendarId, oneDate.year + y);
        auto dt =
            CreateDateFromCodes(cx, calendarId, cal.get(), eraYear,
                                MonthCode{1}, 1, TemporalOverflow::Constrain);
        if (!dt) {
          return false;
        }
        months += MonthsInYear(dt.get()) * sign;
      }

      // Add months since start of year resp. until end of year.
      auto eraYear = CalendarEraYear(calendarId, oneDate.year + years);
      auto dt = CreateDateFromCodes(cx, calendarId, cal.get(), eraYear,
                                    oneDate.monthCode, 1,
                                    TemporalOverflow::Constrain);
      if (!dt) {
        return false;
      }
      if (sign > 0) {
        months += monthsSinceStartOfYear(dt.get());
      } else {
        months -= monthsUntilEndOfYear(dt.get());
      }

      years = 0;
    }

    eraYear = CalendarEraYear(calendarId, constrainedDate.year);
    constrained = CreateDateFromCodes(
        cx, calendarId, cal.get(), eraYear, constrainedDate.monthCode,
        constrainedDate.day, TemporalOverflow::Constrain);
    if (!constrained) {
      return false;
    }
    constrainedIso = ToISODate(constrained.get());

    MOZ_ASSERT(CompareISODate(constrainedIso, two) * sign <= 0,
               "constrained doesn't surpass two");
  }

  int64_t days = MakeDay(two) - MakeDay(constrainedIso);

  *result = DateDuration{
      int64_t(years),
      int64_t(months),
      0,
      int64_t(days),
  };
  MOZ_ASSERT(IsValidDuration(*result));
  return true;
}

static bool DifferenceCalendarDate(JSContext* cx, CalendarId calendarId,
                                   const ISODate& one, const ISODate& two,
                                   TemporalUnit largestUnit,
                                   DateDuration* result) {
  // ICU4X doesn't yet provide a public API for CalendarDateUntil.
  //
  // https://github.com/unicode-org/icu4x/issues/3964

  // Delegate to the ISO 8601 calendar for "weeks" and "days". This works
  // because all supported calendars use a 7-days week.
  if (largestUnit >= TemporalUnit::Week) {
    *result = DifferenceISODate(one, two, largestUnit);
    return true;
  }

  switch (calendarId) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Gregorian:
    case CalendarId::Japanese:
    case CalendarId::ROC:
      // Use the ISO 8601 calendar if the calendar system starts its year at the
      // same time as the ISO 8601 calendar and all months exactly match the
      // ISO 8601 calendar months.
      *result = DifferenceISODate(one, two, largestUnit);
      return true;

    case CalendarId::Chinese:
    case CalendarId::Coptic:
    case CalendarId::Dangi:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem:
    case CalendarId::Hebrew:
    case CalendarId::Indian:
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
    case CalendarId::Persian:
      return DifferenceNonISODate(cx, calendarId, one, two, largestUnit,
                                  result);
  }
  MOZ_CRASH("invalid calendar id");
}

/**
 * CalendarDateUntil ( calendar, one, two, largestUnit )
 */
bool js::temporal::CalendarDateUntil(JSContext* cx,
                                     Handle<CalendarValue> calendar,
                                     const ISODate& one, const ISODate& two,
                                     TemporalUnit largestUnit,
                                     DateDuration* result) {
  MOZ_ASSERT(largestUnit <= TemporalUnit::Day);

  auto calendarId = calendar.identifier();

  // Step 1.
  if (calendarId == CalendarId::ISO8601) {
    *result = DifferenceISODate(one, two, largestUnit);
    return true;
  }

  // Step 2.
  return DifferenceCalendarDate(cx, calendarId, one, two, largestUnit, result);
}
