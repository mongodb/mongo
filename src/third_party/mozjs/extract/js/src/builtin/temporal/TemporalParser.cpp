/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/temporal/TemporalParser.h"

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/Range.h"
#include "mozilla/Result.h"
#include "mozilla/Span.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Try.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <stdint.h>
#include <string_view>
#include <type_traits>
#include <utility>

#include "jsnum.h"
#include "NamespaceImports.h"

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Duration.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/TemporalTypes.h"
#include "builtin/temporal/TemporalUnit.h"
#include "gc/Barrier.h"
#include "gc/Tracer.h"
#include "js/ErrorReport.h"
#include "js/friend/ErrorMessages.h"
#include "js/GCAPI.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "util/Text.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/StringType.h"

using namespace js;
using namespace js::temporal;

struct StringName final {
  // Start position and length of this name.
  size_t start = 0;
  size_t length = 0;

  bool present() const { return length > 0; }
};

static JSLinearString* ToString(JSContext* cx, JSString* string,
                                const StringName& name) {
  MOZ_ASSERT(name.present());
  return NewDependentString(cx, string, name.start, name.length);
}

template <typename CharT>
bool EqualCharIgnoreCaseAscii(CharT c1, char c2) {
  if constexpr (sizeof(CharT) > sizeof(char)) {
    if (!mozilla::IsAscii(c1)) {
      return false;
    }
  }

  static constexpr auto toLower = 0x20;
  static_assert('a' - 'A' == toLower);

  // Convert both characters to lower case before the comparison.
  char c = c1;
  if (mozilla::IsAsciiUppercaseAlpha(c1)) {
    c = char(c + toLower);
  }
  char d = c2;
  if (mozilla::IsAsciiUppercaseAlpha(c2)) {
    d = char(d + toLower);
  }
  return c == d;
}

using CalendarName = StringName;
using AnnotationKey = StringName;
using AnnotationValue = StringName;
using TimeZoneName = StringName;

struct Annotation final {
  AnnotationKey key;
  AnnotationValue value;
  bool critical = false;
};

struct TimeSpec final {
  Time time;
};

struct TimeZoneUTCOffset final {
  // ±1 for time zones with an offset, otherwise 0.
  int32_t sign = 0;

  // An integer in the range [0, 23].
  int32_t hour = 0;

  // An integer in the range [0, 59].
  int32_t minute = 0;
};

struct DateTimeUTCOffset final {
  // ±1 for time zones with an offset, otherwise 0.
  int32_t sign = 0;

  // An integer in the range [0, 23].
  int32_t hour = 0;

  // An integer in the range [0, 59].
  int32_t minute = 0;

  // An integer in the range [0, 59].
  int32_t second = 0;

  // An integer in the range [0, 999'999].
  int32_t fractionalPart = 0;

  // Time zone with sub-minute precision.
  bool subMinutePrecision = false;

  // Convert to a TimeZoneUTCOffset.
  TimeZoneUTCOffset toTimeZoneUTCOffset() const {
    MOZ_ASSERT(!subMinutePrecision, "unexpected sub-minute precision");
    return {sign, hour, minute};
  }
};

/**
 * ParseDateTimeUTCOffset ( offsetString )
 */
static int64_t ParseDateTimeUTCOffset(const DateTimeUTCOffset& offset) {
  constexpr int64_t nanoPerSec = 1'000'000'000;

  MOZ_ASSERT(offset.sign == -1 || offset.sign == +1);
  MOZ_ASSERT(0 <= offset.hour && offset.hour < 24);
  MOZ_ASSERT(0 <= offset.minute && offset.minute < 60);
  MOZ_ASSERT(0 <= offset.second && offset.second < 60);
  MOZ_ASSERT(0 <= offset.fractionalPart && offset.fractionalPart < nanoPerSec);

  // sign × (((hours × 60 + minutes) × 60 + seconds) × 10^9 + nanoseconds).
  int64_t seconds = (offset.hour * 60 + offset.minute) * 60 + offset.second;
  int64_t nanos = (seconds * nanoPerSec) + offset.fractionalPart;
  int64_t result = offset.sign * nanos;

  MOZ_ASSERT(std::abs(result) < ToNanoseconds(TemporalUnit::Day),
             "time zone offset is less than 24:00 hours");

  return result;
}

static int32_t ParseTimeZoneOffset(const TimeZoneUTCOffset& offset) {
  MOZ_ASSERT(offset.sign == -1 || offset.sign == +1);
  MOZ_ASSERT(0 <= offset.hour && offset.hour < 24);
  MOZ_ASSERT(0 <= offset.minute && offset.minute < 60);

  // sign × (hour × 60 + minute).
  int32_t result = offset.sign * (offset.hour * 60 + offset.minute);

  MOZ_ASSERT(std::abs(result) < UnitsPerDay(TemporalUnit::Minute),
             "time zone offset is less than 24:00 hours");

  return result;
}

/**
 * Struct to hold time zone annotations.
 */
struct TimeZoneAnnotation final {
  // Time zone offset.
  TimeZoneUTCOffset offset;

  // Time zone name.
  TimeZoneName name;

  /**
   * Returns true iff the time zone has an offset part, e.g. "+01:00".
   */
  bool hasOffset() const { return offset.sign != 0; }

  /**
   * Returns true iff the time zone has an IANA name, e.g. "Asia/Tokyo".
   */
  bool hasName() const { return name.present(); }
};

/**
 * Struct to hold any time zone parts of a parsed string.
 */
struct TimeZoneString final {
  // Date-time UTC offset.
  DateTimeUTCOffset offset;

  // Time zone annotation;
  TimeZoneAnnotation annotation;

  // UTC time zone.
  bool utc = false;

  static auto from(DateTimeUTCOffset offset) {
    TimeZoneString timeZone{};
    timeZone.offset = offset;
    return timeZone;
  }

  static auto from(TimeZoneUTCOffset offset) {
    TimeZoneString timeZone{};
    timeZone.annotation.offset = offset;
    return timeZone;
  }

  static auto from(TimeZoneName name) {
    TimeZoneString timeZone{};
    timeZone.annotation.name = name;
    return timeZone;
  }

  static auto UTC() {
    TimeZoneString timeZone{};
    timeZone.utc = true;
    return timeZone;
  }

  /**
   * Returns true iff the time zone has an offset part, e.g. "+01:00".
   */
  bool hasOffset() const { return offset.sign != 0; }

  /**
   * Returns true iff the time zone has an annotation.
   */
  bool hasAnnotation() const {
    return annotation.hasName() || annotation.hasOffset();
  }

  /**
   * Returns true iff the time zone uses the "Z" abbrevation to denote UTC time.
   */
  bool isUTC() const { return utc; }
};

/**
 * Struct to hold the parsed date, time, time zone, and calendar components.
 */
struct ZonedDateTimeString final {
  ISODate date;
  Time time;
  TimeZoneString timeZone;
  CalendarName calendar;
  bool startOfDay;
};

template <typename CharT>
static bool IsISO8601Calendar(mozilla::Span<const CharT> calendar) {
  static constexpr std::string_view iso8601 = "iso8601";

  if (calendar.size() != iso8601.length()) {
    return false;
  }

  for (size_t i = 0; i < iso8601.length(); i++) {
    if (!EqualCharIgnoreCaseAscii(calendar[i], iso8601[i])) {
      return false;
    }
  }
  return true;
}

static constexpr int32_t AbsentYear = INT32_MAX;

/**
 * ParseISODateTime ( isoString )
 */
static bool ParseISODateTime(JSContext* cx, const ZonedDateTimeString& parsed,
                             ISODateTime* result) {
  // Steps 1-7, 9, 11-16 (Not applicable here).

  ISODateTime dateTime = {parsed.date, parsed.time};

  // NOTE: ToIntegerOrInfinity("") is 0.
  if (dateTime.date.year == AbsentYear) {
    dateTime.date.year = 0;
  }

  // Step 8.
  if (dateTime.date.month == 0) {
    dateTime.date.month = 1;
  }

  // Step 10.
  if (dateTime.date.day == 0) {
    dateTime.date.day = 1;
  }

  // Step 17.b.
  if (dateTime.time.second == 60) {
    dateTime.time.second = 59;
  }

  // ParseISODateTime, steps 18-19 (Not applicable in our implementation).

  // Perform early error checks now that absent |day| and |month| values were
  // handled:
  // `IsValidDate(DateSpec)` and `IsValidMonthDay(DateSpecMonthDay)` validate
  // that |day| doesn't exceed the number of days in |month|. This check can be
  // implemented by calling `ThrowIfInvalidISODate`.
  //
  // All other values are already in-bounds.
  MOZ_ASSERT(std::abs(dateTime.date.year) <= 999'999);
  MOZ_ASSERT(1 <= dateTime.date.month && dateTime.date.month <= 12);
  MOZ_ASSERT(1 <= dateTime.date.day && dateTime.date.day <= 31);

  if (!ThrowIfInvalidISODate(cx, dateTime.date)) {
    return false;
  }

  // Step 20.
  MOZ_ASSERT(IsValidISODate(dateTime.date));

  // Step 21.
  MOZ_ASSERT(IsValidTime(dateTime.time));

  // Steps 22-28. (Handled in caller.)

  *result = dateTime;
  return true;
}

static bool ParseTimeZoneAnnotation(JSContext* cx,
                                    const TimeZoneAnnotation& annotation,
                                    JSLinearString* linear,
                                    MutableHandle<ParsedTimeZone> result) {
  MOZ_ASSERT(annotation.hasOffset() || annotation.hasName());

  if (annotation.hasOffset()) {
    int32_t offset = ParseTimeZoneOffset(annotation.offset);
    result.set(ParsedTimeZone::fromOffset(offset));
    return true;
  }

  auto* str = ToString(cx, linear, annotation.name);
  if (!str) {
    return false;
  }
  result.set(ParsedTimeZone::fromName(str));
  return true;
}

/**
 * Struct for the parsed duration components.
 */
struct TemporalDurationString final {
  // A non-negative integer or +Infinity.
  double years = 0;

  // A non-negative integer or +Infinity.
  double months = 0;

  // A non-negative integer or +Infinity.
  double weeks = 0;

  // A non-negative integer or +Infinity.
  double days = 0;

  // A non-negative integer or +Infinity.
  double hours = 0;

  // A non-negative integer or +Infinity.
  double minutes = 0;

  // A non-negative integer or +Infinity.
  double seconds = 0;

  // An integer in the range [0, 999'999].
  int32_t hoursFraction = 0;

  // An integer in the range [0, 999'999].
  int32_t minutesFraction = 0;

  // An integer in the range [0, 999'999].
  int32_t secondsFraction = 0;

  // ±1 when an offset is present, otherwise 0.
  int32_t sign = 0;
};

class ParserError final {
  JSErrNum error_ = JSMSG_NOT_AN_ERROR;

 public:
  constexpr ParserError() = default;

  constexpr MOZ_IMPLICIT ParserError(JSErrNum error) : error_(error) {}

  constexpr JSErrNum error() const { return error_; }

  constexpr operator JSErrNum() const { return error(); }
};

namespace mozilla::detail {
// Zero is used for tagging, so it mustn't be an error.
static_assert(static_cast<JSErrNum>(0) == JSMSG_NOT_AN_ERROR);

// Ensure efficient packing of the error type.
template <>
struct UnusedZero<::ParserError> {
 private:
  using Error = ::ParserError;
  using ErrorKind = JSErrNum;

 public:
  using StorageType = std::underlying_type_t<ErrorKind>;

  static constexpr bool value = true;
  static constexpr StorageType nullValue = 0;

  static constexpr Error Inspect(const StorageType& aValue) {
    return Error(static_cast<ErrorKind>(aValue));
  }
  static constexpr Error Unwrap(StorageType aValue) {
    return Error(static_cast<ErrorKind>(aValue));
  }
  static constexpr StorageType Store(Error aValue) {
    return static_cast<StorageType>(aValue.error());
  }
};
}  // namespace mozilla::detail

static_assert(mozilla::Result<ZonedDateTimeString, ParserError>::Strategy !=
              mozilla::detail::PackingStrategy::Variant);

/**
 * Track the error and reader index of the last largest successful parse.
 */
class LikelyError final {
  size_t index_ = 0;
  ParserError error_{};

 public:
  template <typename V>
  void update(const mozilla::Result<V, ParserError>& result, size_t index) {
    MOZ_ASSERT(result.isErr());

    if (index >= index_) {
      index_ = index;
      error_ = result.inspectErr();
    }
  }

  size_t index() const { return index_; }

  auto propagate() const { return mozilla::Err(error_); }
};

template <typename CharT>
class StringReader final {
  mozilla::Span<const CharT> string_;

  // Current position in the string.
  size_t index_ = 0;

 public:
  explicit StringReader(mozilla::Span<const CharT> string) : string_(string) {}

  /**
   * Returns the input string.
   */
  mozilla::Span<const CharT> string() const { return string_; }

  /**
   * Returns a substring of the input string.
   */
  mozilla::Span<const CharT> substring(const StringName& name) const {
    MOZ_ASSERT(name.present());
    return string_.Subspan(name.start, name.length);
  }

  /**
   * Returns the current parse position.
   */
  size_t index() const { return index_; }

  /**
   * Returns the length of the input string-
   */
  size_t length() const { return string_.size(); }

  /**
   * Returns true iff the whole string has been parsed.
   */
  bool atEnd() const { return index() == length(); }

  /**
   * Reset the parser to a previous parse position.
   */
  void reset(size_t index = 0) {
    MOZ_ASSERT(index <= length());
    index_ = index;
  }

  /**
   * Returns true if at least `amount` characters can be read from the current
   * parse position.
   */
  bool hasMore(size_t amount) const { return index() + amount <= length(); }

  /**
   * Advances the parse position by `amount` characters.
   */
  void advance(size_t amount) {
    MOZ_ASSERT(hasMore(amount));
    index_ += amount;
  }

  /**
   * Returns the character at the current parse position.
   */
  CharT current() const { return string()[index()]; }

  /**
   * Returns the character at the next parse position.
   */
  CharT next() const { return string()[index() + 1]; }

  /**
   * Returns the character at position `index`.
   */
  CharT at(size_t index) const { return string()[index]; }
};

template <typename CharT>
class TemporalParser final {
  StringReader<CharT> reader_;

  /**
   * Read an unlimited amount of decimal digits, returning `Nothing` if no
   * digits were read.
   */
  mozilla::Maybe<double> digits(JSContext* cx);

  /**
   * Read exactly `length` digits, returning `Nothing` on failure.
   */
  mozilla::Maybe<int32_t> digits(size_t length) {
    MOZ_ASSERT(length > 0, "can't read zero digits");
    MOZ_ASSERT(length <= std::numeric_limits<int32_t>::digits10,
               "can't read more than digits10 digits without overflow");

    if (!reader_.hasMore(length)) {
      return mozilla::Nothing();
    }
    int32_t num = 0;
    size_t index = reader_.index();
    for (size_t i = 0; i < length; i++) {
      auto ch = reader_.at(index + i);
      if (!mozilla::IsAsciiDigit(ch)) {
        return mozilla::Nothing();
      }
      num = num * 10 + AsciiDigitToNumber(ch);
    }
    reader_.advance(length);
    return mozilla::Some(num);
  }

  // TemporalDecimalFraction :::
  //   TemporalDecimalSeparator DecimalDigit{1,9}
  mozilla::Maybe<int32_t> fraction() {
    if (!reader_.hasMore(2)) {
      return mozilla::Nothing();
    }
    if (!hasDecimalSeparator() || !mozilla::IsAsciiDigit(reader_.next())) {
      return mozilla::Nothing();
    }

    // Consume the decimal separator.
    MOZ_ALWAYS_TRUE(decimalSeparator());

    // Maximal nine fractional digits are supported.
    constexpr size_t maxFractions = 9;

    // Read up to |maxFractions| digits.
    int32_t num = 0;
    size_t index = reader_.index();
    size_t i = 0;
    for (; i < std::min(reader_.length() - index, maxFractions); i++) {
      CharT ch = reader_.at(index + i);
      if (!mozilla::IsAsciiDigit(ch)) {
        break;
      }
      num = num * 10 + AsciiDigitToNumber(ch);
    }

    // Skip past the read digits.
    reader_.advance(i);

    // Normalize the fraction to |maxFractions| digits.
    for (; i < maxFractions; i++) {
      num *= 10;
    }
    return mozilla::Some(num);
  }

  /**
   * Returns true iff the current character is `ch`.
   */
  bool hasCharacter(CharT ch) const {
    return reader_.hasMore(1) && reader_.current() == ch;
  }

  /**
   * Consumes the current character if it's equal to `ch` and then returns
   * `true`. Otherwise returns `false`.
   */
  bool character(CharT ch) {
    if (!hasCharacter(ch)) {
      return false;
    }
    reader_.advance(1);
    return true;
  }

  /**
   * Consumes the next characters if they're equal to `str` and then returns
   * `true`. Otherwise returns `false`.
   */
  template <size_t N>
  bool string(const char (&str)[N]) {
    static_assert(N > 2, "use character() for one element strings");

    if (!reader_.hasMore(N - 1)) {
      return false;
    }
    size_t index = reader_.index();
    for (size_t i = 0; i < N - 1; i++) {
      if (reader_.at(index + i) != str[i]) {
        return false;
      }
    }
    reader_.advance(N - 1);
    return true;
  }

  /**
   * Returns true if the next two characters are ASCII alphabetic characters.
   */
  bool hasTwoAsciiAlpha() {
    if (!reader_.hasMore(2)) {
      return false;
    }
    size_t index = reader_.index();
    return mozilla::IsAsciiAlpha(reader_.at(index)) &&
           mozilla::IsAsciiAlpha(reader_.at(index + 1));
  }

  /**
   * Returns true iff the current character is one of `chars`.
   */
  bool hasOneOf(std::initializer_list<char16_t> chars) const {
    if (!reader_.hasMore(1)) {
      return false;
    }
    auto ch = reader_.current();
    return std::find(chars.begin(), chars.end(), ch) != chars.end();
  }

  /**
   * Consumes the current character if it's in `chars` and then returns `true`.
   * Otherwise returns `false`.
   */
  bool oneOf(std::initializer_list<char16_t> chars) {
    if (!hasOneOf(chars)) {
      return false;
    }
    reader_.advance(1);
    return true;
  }

  /**
   * Consumes the current character if it matches the predicate and then returns
   * `true`. Otherwise returns `false`.
   */
  template <typename Predicate>
  bool matches(Predicate&& predicate) {
    if (!reader_.hasMore(1)) {
      return false;
    }

    CharT ch = reader_.current();
    if (!predicate(ch)) {
      return false;
    }

    reader_.advance(1);
    return true;
  }

  // ASCIISign ::: one of
  //   + -
  bool hasSign() const { return hasOneOf({'+', '-'}); }

  /**
   * Consumes the current character, which must be a sign character, and returns
   * its numeric value.
   */
  int32_t sign() {
    MOZ_ASSERT(hasSign());
    int32_t plus = hasCharacter('+');
    reader_.advance(1);
    return plus ? 1 : -1;
  }

  // DateSeparator[Extended] :::
  //   [+Extended] -
  //   [~Extended] [empty]
  bool dateSeparator() { return character('-'); }

  // TimeSeparator[Extended] :::
  //   [+Extended] :
  //   [~Extended] [empty]
  bool hasTimeSeparator() const { return hasCharacter(':'); }

  bool timeSeparator() { return character(':'); }

  // TemporalDecimalSeparator ::: one of
  //   . ,
  bool hasDecimalSeparator() const { return hasOneOf({'.', ','}); }

  bool decimalSeparator() { return oneOf({'.', ','}); }

  // DaysDesignator ::: one of
  //   D d
  bool daysDesignator() { return oneOf({'D', 'd'}); }

  // HoursDesignator ::: one of
  //   H h
  bool hoursDesignator() { return oneOf({'H', 'h'}); }

  // MinutesDesignator ::: one of
  //   M m
  bool minutesDesignator() { return oneOf({'M', 'm'}); }

  // MonthsDesignator ::: one of
  //   M m
  bool monthsDesignator() { return oneOf({'M', 'm'}); }

  // DurationDesignator ::: one of
  //   P p
  bool durationDesignator() { return oneOf({'P', 'p'}); }

  // SecondsDesignator ::: one of
  //   S s
  bool secondsDesignator() { return oneOf({'S', 's'}); }

  // DateTimeSeparator :::
  //   <SP>
  //   T
  //   t
  bool dateTimeSeparator() { return oneOf({' ', 'T', 't'}); }

  // TimeDesignator ::: one of
  //   T t
  bool hasTimeDesignator() const { return hasOneOf({'T', 't'}); }

  bool timeDesignator() { return oneOf({'T', 't'}); }

  // WeeksDesignator ::: one of
  //   W w
  bool weeksDesignator() { return oneOf({'W', 'w'}); }

  // YearsDesignator ::: one of
  //   Y y
  bool yearsDesignator() { return oneOf({'Y', 'y'}); }

  // UTCDesignator ::: one of
  //   Z z
  bool utcDesignator() { return oneOf({'Z', 'z'}); }

  // TZLeadingChar :::
  //   Alpha
  //   .
  //   _
  bool tzLeadingChar() {
    return matches([](auto ch) {
      return mozilla::IsAsciiAlpha(ch) || ch == '.' || ch == '_';
    });
  }

  // TZChar :::
  //   TZLeadingChar
  //   DecimalDigit
  //   -
  //   +
  bool tzChar() {
    return matches([](auto ch) {
      return mozilla::IsAsciiAlphanumeric(ch) || ch == '.' || ch == '_' ||
             ch == '-' || ch == '+';
    });
  }

  // AnnotationCriticalFlag :::
  //   !
  bool annotationCriticalFlag() { return character('!'); }

  // AKeyLeadingChar :::
  //   LowercaseAlpha
  //   _
  bool aKeyLeadingChar() {
    return matches([](auto ch) {
      return mozilla::IsAsciiLowercaseAlpha(ch) || ch == '_';
    });
  }

  // AKeyChar :::
  //   AKeyLeadingChar
  //   DecimalDigit
  //   -
  bool aKeyChar() {
    return matches([](auto ch) {
      return mozilla::IsAsciiLowercaseAlpha(ch) || mozilla::IsAsciiDigit(ch) ||
             ch == '-' || ch == '_';
    });
  }

  // AnnotationValueComponent :::
  //   Alpha AnnotationValueComponent?
  //   DecimalDigit AnnotationValueComponent?
  bool annotationValueComponent() {
    size_t index = reader_.index();
    size_t i = 0;
    for (; index + i < reader_.length(); i++) {
      auto ch = reader_.at(index + i);
      if (!mozilla::IsAsciiAlphanumeric(ch)) {
        break;
      }
    }
    if (i == 0) {
      return false;
    }
    reader_.advance(i);
    return true;
  }

  template <typename T>
  static constexpr bool inBounds(const T& x, const T& min, const T& max) {
    return min <= x && x <= max;
  }

  static auto err(JSErrNum error) {
    // Explicitly create |ParserError| when JSErrNum is auto-convertible to the
    // success type.
    return mozilla::Err(ParserError{error});
  }

  mozilla::Result<int32_t, ParserError> dateYear();
  mozilla::Result<int32_t, ParserError> dateMonth();
  mozilla::Result<int32_t, ParserError> dateDay();
  mozilla::Result<int32_t, ParserError> hour();
  mozilla::Result<mozilla::Maybe<int32_t>, ParserError> minute(bool required);
  mozilla::Result<mozilla::Maybe<int32_t>, ParserError> second(bool required);
  mozilla::Result<mozilla::Maybe<int32_t>, ParserError> timeSecond(
      bool required);

  mozilla::Result<ISODate, ParserError> date();

  mozilla::Result<Time, ParserError> time();

  mozilla::Result<ZonedDateTimeString, ParserError> dateTime(bool allowZ);

  mozilla::Result<ISODate, ParserError> dateSpecYearMonth();

  mozilla::Result<ISODate, ParserError> dateSpecMonthDay();

  // Return true when |Annotation| can start at the current position.
  bool hasAnnotationStart() const { return hasCharacter('['); }

  // Return true when |TimeZoneAnnotation| can start at the current position.
  bool hasTimeZoneAnnotationStart() const {
    if (!hasCharacter('[')) {
      return false;
    }

    // Ensure no '=' is found before the closing ']', otherwise the opening '['
    // may actually start an |Annotation| instead of a |TimeZoneAnnotation|.
    for (size_t i = reader_.index() + 1; i < reader_.length(); i++) {
      CharT ch = reader_.at(i);
      if (ch == '=') {
        return false;
      }
      if (ch == ']') {
        break;
      }
    }
    return true;
  }

  // Return true when |DateTimeUTCOffset| can start at the current position.
  bool hasDateTimeUTCOffsetStart() { return hasOneOf({'Z', 'z', '+', '-'}); }

  mozilla::Result<TimeZoneString, ParserError> dateTimeUTCOffset(bool allowZ);

  mozilla::Result<DateTimeUTCOffset, ParserError> utcOffsetSubMinutePrecision();

  mozilla::Result<TimeZoneUTCOffset, ParserError> timeZoneUTCOffsetName();

  mozilla::Result<TimeZoneAnnotation, ParserError> timeZoneIdentifier();

  mozilla::Result<TimeZoneAnnotation, ParserError> timeZoneAnnotation();

  mozilla::Result<TimeZoneName, ParserError> timeZoneIANAName();

  mozilla::Result<AnnotationKey, ParserError> annotationKey();
  mozilla::Result<AnnotationValue, ParserError> annotationValue();
  mozilla::Result<Annotation, ParserError> annotation();
  mozilla::Result<CalendarName, ParserError> annotations();

  mozilla::Result<ZonedDateTimeString, ParserError> annotatedTime();

  mozilla::Result<ZonedDateTimeString, ParserError> annotatedDateTime();

  mozilla::Result<ZonedDateTimeString, ParserError>
  annotatedDateTimeTimeRequired();

  mozilla::Result<ZonedDateTimeString, ParserError> annotatedYearMonth();

  mozilla::Result<ZonedDateTimeString, ParserError> annotatedMonthDay();

  mozilla::Result<double, ParserError> durationDigits(JSContext* cx);

  template <typename T>
  mozilla::Result<T, ParserError> parse(
      mozilla::Result<T, ParserError>&& result) const;

  template <typename T>
  mozilla::Result<T, ParserError> complete(const T& value) const;

  mozilla::Result<mozilla::Ok, ParserError> nonempty() const;

 public:
  explicit TemporalParser(mozilla::Span<const CharT> str) : reader_(str) {}

  mozilla::Result<ZonedDateTimeString, ParserError>
  parseTemporalInstantString();

  mozilla::Result<ZonedDateTimeString, ParserError>
  parseTemporalTimeZoneString();

  mozilla::Result<TimeZoneAnnotation, ParserError> parseTimeZoneIdentifier();

  mozilla::Result<DateTimeUTCOffset, ParserError> parseDateTimeUTCOffset();

  mozilla::Result<TemporalDurationString, ParserError>
  parseTemporalDurationString(JSContext* cx);

  mozilla::Result<ZonedDateTimeString, ParserError>
  parseTemporalCalendarString();

  mozilla::Result<ZonedDateTimeString, ParserError> parseTemporalTimeString();

  mozilla::Result<ZonedDateTimeString, ParserError>
  parseTemporalMonthDayString();

  mozilla::Result<ZonedDateTimeString, ParserError>
  parseTemporalYearMonthString();

  mozilla::Result<ZonedDateTimeString, ParserError>
  parseTemporalDateTimeString();

  mozilla::Result<ZonedDateTimeString, ParserError>
  parseTemporalZonedDateTimeString();

  mozilla::Result<ZonedDateTimeString, ParserError>
  parseTemporalRelativeToString();
};

template <typename CharT>
template <typename T>
mozilla::Result<T, ParserError> TemporalParser<CharT>::parse(
    mozilla::Result<T, ParserError>&& result) const {
  if (result.isOk() && !reader_.atEnd()) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_UNEXPECTED_CHARACTERS_AT_END);
  }
  return std::move(result);
}

template <typename CharT>
template <typename T>
mozilla::Result<T, ParserError> TemporalParser<CharT>::complete(
    const T& result) const {
  if (!reader_.atEnd()) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_UNEXPECTED_CHARACTERS_AT_END);
  }
  return result;
}

template <typename CharT>
mozilla::Result<mozilla::Ok, ParserError> TemporalParser<CharT>::nonempty()
    const {
  if (reader_.length() == 0) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_EMPTY_STRING);
  }
  return mozilla::Ok{};
}

template <typename CharT>
mozilla::Result<int32_t, ParserError> TemporalParser<CharT>::dateYear() {
  // DateYear :::
  //  DecimalDigit{4}
  //  ASCIISign DecimalDigit{6}

  if (auto year = digits(4)) {
    return year.value();
  }
  if (hasSign()) {
    int32_t yearSign = sign();
    if (auto year = digits(6)) {
      int32_t result = yearSign * year.value();
      if (yearSign < 0 && result == 0) {
        return err(JSMSG_TEMPORAL_PARSER_NEGATIVE_ZERO_YEAR);
      }
      return result;
    }
    return err(JSMSG_TEMPORAL_PARSER_MISSING_EXTENDED_YEAR);
  }
  return err(JSMSG_TEMPORAL_PARSER_MISSING_YEAR);
}

template <typename CharT>
mozilla::Result<int32_t, ParserError> TemporalParser<CharT>::dateMonth() {
  // DateMonth :::
  //   0 NonzeroDigit
  //   10
  //   11
  //   12
  if (auto month = digits(2)) {
    int32_t result = month.value();
    if (!inBounds(result, 1, 12)) {
      return err(JSMSG_TEMPORAL_PARSER_INVALID_MONTH);
    }
    return result;
  }
  return err(JSMSG_TEMPORAL_PARSER_MISSING_MONTH);
}

template <typename CharT>
mozilla::Result<int32_t, ParserError> TemporalParser<CharT>::dateDay() {
  // DateDay :::
  //   0 NonzeroDigit
  //   1 DecimalDigit
  //   2 DecimalDigit
  //   30
  //   31
  if (auto day = digits(2)) {
    int32_t result = day.value();
    if (!inBounds(result, 1, 31)) {
      return err(JSMSG_TEMPORAL_PARSER_INVALID_DAY);
    }
    return result;
  }
  return err(JSMSG_TEMPORAL_PARSER_MISSING_DAY);
}

template <typename CharT>
mozilla::Result<int32_t, ParserError> TemporalParser<CharT>::hour() {
  // Hour :::
  //   0 DecimalDigit
  //   1 DecimalDigit
  //   20
  //   21
  //   22
  //   23
  if (auto hour = digits(2)) {
    int32_t result = hour.value();
    if (!inBounds(result, 0, 23)) {
      return err(JSMSG_TEMPORAL_PARSER_INVALID_HOUR);
    }
    return result;
  }
  return err(JSMSG_TEMPORAL_PARSER_MISSING_HOUR);
}

template <typename CharT>
mozilla::Result<mozilla::Maybe<int32_t>, ParserError>
TemporalParser<CharT>::minute(bool required) {
  // MinuteSecond :::
  //   0 DecimalDigit
  //   1 DecimalDigit
  //   2 DecimalDigit
  //   3 DecimalDigit
  //   4 DecimalDigit
  //   5 DecimalDigit
  if (auto minute = digits(2)) {
    if (!inBounds(minute.value(), 0, 59)) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_MINUTE);
    }
    return minute;
  }
  if (!required) {
    return mozilla::Maybe<int32_t>{mozilla::Nothing{}};
  }
  return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_MINUTE);
}

template <typename CharT>
mozilla::Result<mozilla::Maybe<int32_t>, ParserError>
TemporalParser<CharT>::second(bool required) {
  // MinuteSecond :::
  //   0 DecimalDigit
  //   1 DecimalDigit
  //   2 DecimalDigit
  //   3 DecimalDigit
  //   4 DecimalDigit
  //   5 DecimalDigit
  if (auto minute = digits(2)) {
    if (!inBounds(minute.value(), 0, 59)) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_SECOND);
    }
    return minute;
  }
  if (!required) {
    return mozilla::Maybe<int32_t>{mozilla::Nothing{}};
  }
  return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_SECOND);
}

template <typename CharT>
mozilla::Result<mozilla::Maybe<int32_t>, ParserError>
TemporalParser<CharT>::timeSecond(bool required) {
  // TimeSecond :::
  //   MinuteSecond
  //   60
  //
  // MinuteSecond :::
  //   0 DecimalDigit
  //   1 DecimalDigit
  //   2 DecimalDigit
  //   3 DecimalDigit
  //   4 DecimalDigit
  //   5 DecimalDigit
  if (auto minute = digits(2)) {
    if (!inBounds(minute.value(), 0, 60)) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_LEAPSECOND);
    }
    return minute;
  }
  if (!required) {
    return mozilla::Maybe<int32_t>{mozilla::Nothing{}};
  }
  return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_SECOND);
}

template <typename CharT>
mozilla::Result<ISODate, ParserError> TemporalParser<CharT>::date() {
  // clang-format off
  //
  // Date :::
  //   DateSpec[+Extended]
  //   DateSpec[~Extended]
  //
  // DateSpec[Extended] :::
  //   DateYear DateSeparator[?Extended] DateMonth DateSeparator[?Extended] DateDay
  //
  // clang-format on

  ISODate result{};

  MOZ_TRY_VAR(result.year, dateYear());

  // Optional |DateSeparator|.
  bool hasMonthSeparator = dateSeparator();

  MOZ_TRY_VAR(result.month, dateMonth());

  // Optional |DateSeparator|.
  bool hasDaySeparator = dateSeparator();

  // Date separators must be consistent.
  if (hasMonthSeparator != hasDaySeparator) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_INCONSISTENT_DATE_SEPARATOR);
  }

  MOZ_TRY_VAR(result.day, dateDay());

  return result;
}

template <typename CharT>
mozilla::Result<Time, ParserError> TemporalParser<CharT>::time() {
  // clang-format off
  //
  // Time :::
  //   TimeSpec[+Extended]
  //   TimeSpec[~Extended]
  //
  // TimeSpec[Extended] :::
  //   Hour
  //   Hour TimeSeparator[?Extended] MinuteSecond
  //   Hour TimeSeparator[?Extended] MinuteSecond TimeSeparator[?Extended] TimeSecond TemporalDecimalFraction?
  //
  // clang-format on

  Time result{};

  MOZ_TRY_VAR(result.hour, hour());

  // Optional |TimeSeparator|.
  bool hasMinuteSeparator = timeSeparator();

  mozilla::Maybe<int32_t> minutes;
  MOZ_TRY_VAR(minutes, minute(hasMinuteSeparator));
  if (minutes) {
    result.minute = minutes.value();

    // Optional |TimeSeparator|.
    bool hasSecondSeparator = timeSeparator();

    mozilla::Maybe<int32_t> seconds;
    MOZ_TRY_VAR(seconds, timeSecond(hasSecondSeparator));
    if (seconds) {
      result.second = seconds.value();

      // Time separators must be consistent.
      if (hasMinuteSeparator != hasSecondSeparator) {
        return mozilla::Err(JSMSG_TEMPORAL_PARSER_INCONSISTENT_TIME_SEPARATOR);
      }

      // TemporalDecimalFraction :::
      //   TemporalDecimalSeparator DecimalDigit{1,9}
      if (auto f = fraction()) {
        int32_t fractionalPart = f.value();
        result.millisecond = fractionalPart / 1'000'000;
        result.microsecond = (fractionalPart % 1'000'000) / 1'000;
        result.nanosecond = fractionalPart % 1'000;
      }
    }
  }

  return result;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::dateTime(bool allowZ) {
  // DateTime[Z, TimeRequired] :::
  //   [~TimeRequired] Date
  //   Date DateTimeSeparator Time DateTimeUTCOffset[?Z]?
  //
  // When called as `DateTime[?Z, ~TimeRequired]`.

  ZonedDateTimeString result{};

  MOZ_TRY_VAR(result.date, date());

  if (dateTimeSeparator()) {
    MOZ_TRY_VAR(result.time, time());

    if (hasDateTimeUTCOffsetStart()) {
      MOZ_TRY_VAR(result.timeZone, dateTimeUTCOffset(allowZ));
    }
  } else {
    result.startOfDay = true;
  }

  return result;
}

template <typename CharT>
mozilla::Result<TimeZoneString, ParserError>
TemporalParser<CharT>::dateTimeUTCOffset(bool allowZ) {
  // DateTimeUTCOffset[Z] :::
  //   [+Z] UTCDesignator
  //   UTCOffset[+SubMinutePrecision]

  if (utcDesignator()) {
    if (!allowZ) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_UTC_DESIGNATOR);
    }
    return TimeZoneString::UTC();
  }

  if (hasSign()) {
    DateTimeUTCOffset offset;
    MOZ_TRY_VAR(offset, utcOffsetSubMinutePrecision());

    return TimeZoneString::from(offset);
  }

  return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_TIMEZONE);
}

template <typename CharT>
mozilla::Result<TimeZoneUTCOffset, ParserError>
TemporalParser<CharT>::timeZoneUTCOffsetName() {
  // clang-format off
  //
  // UTCOffset[SubMinutePrecision] :::
  //   ASCIISign Hour
  //   ASCIISign Hour TimeSeparator[+Extended] MinuteSecond
  //   ASCIISign Hour TimeSeparator[~Extended] MinuteSecond
  //   [+SubMinutePrecision] ASCIISign Hour TimeSeparator[+Extended] MinuteSecond TimeSeparator[+Extended] MinuteSecond TemporalDecimalFraction?
  //   [+SubMinutePrecision] ASCIISign Hour TimeSeparator[~Extended] MinuteSecond TimeSeparator[~Extended] MinuteSecond TemporalDecimalFraction?
  //
  // When called as `UTCOffset[~SubMinutePrecision]`.
  //
  // clang-format on

  TimeZoneUTCOffset result{};

  if (!hasSign()) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_TIMEZONE_SIGN);
  }
  result.sign = sign();

  MOZ_TRY_VAR(result.hour, hour());

  // Optional |TimeSeparator|.
  bool hasMinuteSeparator = timeSeparator();

  mozilla::Maybe<int32_t> minutes;
  MOZ_TRY_VAR(minutes, minute(hasMinuteSeparator));
  if (minutes) {
    result.minute = minutes.value();

    if (hasTimeSeparator()) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_SUBMINUTE_TIMEZONE);
    }
  }

  return result;
}

template <typename CharT>
mozilla::Result<DateTimeUTCOffset, ParserError>
TemporalParser<CharT>::utcOffsetSubMinutePrecision() {
  // clang-format off
  //
  // UTCOffset[SubMinutePrecision] :::
  //   ASCIISign Hour
  //   ASCIISign Hour TimeSeparator[+Extended] MinuteSecond
  //   ASCIISign Hour TimeSeparator[~Extended] MinuteSecond
  //   [+SubMinutePrecision] ASCIISign Hour TimeSeparator[+Extended] MinuteSecond TimeSeparator[+Extended] MinuteSecond TemporalDecimalFraction?
  //   [+SubMinutePrecision] ASCIISign Hour TimeSeparator[~Extended] MinuteSecond TimeSeparator[~Extended] MinuteSecond TemporalDecimalFraction?
  //
  // When called as `UTCOffset[+SubMinutePrecision]`.
  //
  // clang-format on

  DateTimeUTCOffset result{};

  if (!hasSign()) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_TIMEZONE_SIGN);
  }
  result.sign = sign();

  MOZ_TRY_VAR(result.hour, hour());

  // Optional |TimeSeparator|.
  bool hasMinuteSeparator = timeSeparator();

  mozilla::Maybe<int32_t> minutes;
  MOZ_TRY_VAR(minutes, minute(hasMinuteSeparator));
  if (minutes) {
    result.minute = minutes.value();

    // Optional |TimeSeparator|.
    bool hasSecondSeparator = timeSeparator();

    mozilla::Maybe<int32_t> seconds;
    MOZ_TRY_VAR(seconds, second(hasSecondSeparator));
    if (seconds) {
      result.second = seconds.value();

      // Time separators must be consistent.
      if (hasMinuteSeparator != hasSecondSeparator) {
        return mozilla::Err(JSMSG_TEMPORAL_PARSER_INCONSISTENT_TIME_SEPARATOR);
      }

      if (auto fractionalPart = fraction()) {
        result.fractionalPart = fractionalPart.value();
      }

      result.subMinutePrecision = true;
    }
  }

  return result;
}

template <typename CharT>
mozilla::Result<TimeZoneAnnotation, ParserError>
TemporalParser<CharT>::timeZoneIdentifier() {
  // TimeZoneIdentifier :::
  //   UTCOffset[~SubMinutePrecision]
  //   TimeZoneIANAName

  TimeZoneAnnotation result{};
  if (hasSign()) {
    MOZ_TRY_VAR(result.offset, timeZoneUTCOffsetName());
  } else {
    MOZ_TRY_VAR(result.name, timeZoneIANAName());
  }

  return result;
}

template <typename CharT>
mozilla::Result<TimeZoneAnnotation, ParserError>
TemporalParser<CharT>::timeZoneAnnotation() {
  // TimeZoneAnnotation :::
  //   [ AnnotationCriticalFlag? TimeZoneIdentifier ]

  if (!character('[')) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_BRACKET_BEFORE_TIMEZONE);
  }

  // Skip over the optional critical flag.
  annotationCriticalFlag();

  auto result = timeZoneIdentifier();
  if (result.isErr()) {
    return result.propagateErr();
  }

  if (!character(']')) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_BRACKET_AFTER_TIMEZONE);
  }

  return result;
}

template <typename CharT>
mozilla::Result<TimeZoneName, ParserError>
TemporalParser<CharT>::timeZoneIANAName() {
  // TimeZoneIANAName :::
  //   TimeZoneIANANameComponent
  //   TimeZoneIANAName / TimeZoneIANANameComponent
  //
  // TimeZoneIANANameComponent :::
  //   TZLeadingChar
  //   TimeZoneIANANameComponent TZChar

  size_t start = reader_.index();

  do {
    if (!tzLeadingChar()) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_TIMEZONE_NAME);
    }

    // Optionally followed by a sequence of |TZChar|.
    while (tzChar()) {
    }
  } while (character('/'));

  return TimeZoneName{start, reader_.index() - start};
}

template <typename CharT>
mozilla::Maybe<double> TemporalParser<CharT>::digits(JSContext* cx) {
  auto span = reader_.string().Subspan(reader_.index());

  // GetPrefixInteger can't fail when integer separator handling is disabled.
  const CharT* endp = nullptr;
  double num;
  MOZ_ALWAYS_TRUE(GetPrefixInteger(span.data(), span.data() + span.size(), 10,
                                   IntegerSeparatorHandling::None, &endp,
                                   &num));

  size_t len = endp - span.data();
  if (len == 0) {
    return mozilla::Nothing();
  }
  reader_.advance(len);
  return mozilla::Some(num);
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::parseTemporalInstantString() {
  MOZ_TRY(nonempty());

  // Initialize all fields to zero.
  ZonedDateTimeString result{};

  // clang-format off
  //
  // TemporalInstantString :::
  //   Date DateTimeSeparator Time DateTimeUTCOffset[+Z] TimeZoneAnnotation? Annotations?
  //
  // clang-format on

  MOZ_TRY_VAR(result.date, date());

  if (!dateTimeSeparator()) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_DATE_TIME_SEPARATOR);
  }

  MOZ_TRY_VAR(result.time, time());

  MOZ_TRY_VAR(result.timeZone, dateTimeUTCOffset(/* allowZ = */ true));

  if (hasTimeZoneAnnotationStart()) {
    MOZ_TRY_VAR(result.timeZone.annotation, timeZoneAnnotation());
  }

  if (hasAnnotationStart()) {
    MOZ_TRY(annotations());
  }

  return complete(result);
}

/**
 * ParseTemporalInstantString ( isoString )
 */
template <typename CharT>
static auto ParseTemporalInstantString(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTemporalInstantString();
}

/**
 * ParseTemporalInstantString ( isoString )
 */
static auto ParseTemporalInstantString(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTemporalInstantString<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseTemporalInstantString<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseTemporalInstantString ( isoString )
 */
bool js::temporal::ParseTemporalInstantString(JSContext* cx,
                                              Handle<JSString*> str,
                                              ISODateTime* result,
                                              int64_t* offset) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Step 1.
  auto parseResult = ::ParseTemporalInstantString(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "instant");
    return false;
  }
  ZonedDateTimeString parsed = parseResult.unwrap();

  // Step 2.
  if (!ParseISODateTime(cx, parsed, result)) {
    return false;
  }

  // Steps 3-4.
  if (parsed.timeZone.hasOffset()) {
    *offset = ParseDateTimeUTCOffset(parsed.timeZone.offset);
  } else {
    MOZ_ASSERT(parsed.timeZone.isUTC());
    *offset = 0;
  }
  return true;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::parseTemporalTimeZoneString() {
  MOZ_TRY(nonempty());

  // Handle the common case of a standalone time zone identifier first.
  if (auto tz = parse(timeZoneIdentifier()); tz.isOk()) {
    auto timeZone = tz.unwrap();

    ZonedDateTimeString result{};
    if (timeZone.hasOffset()) {
      result.timeZone = TimeZoneString::from(timeZone.offset);
    } else {
      MOZ_ASSERT(timeZone.hasName());
      result.timeZone = TimeZoneString::from(timeZone.name);
    }
    return result;
  }

  LikelyError likelyError{};

  // Try all five parse goals from ParseISODateTime in order.
  //
  // TemporalDateTimeString
  // TemporalInstantString
  // TemporalTimeString
  // TemporalMonthDayString
  // TemporalYearMonthString

  // Restart parsing from the start of the string.
  reader_.reset();

  auto dateTime = parseTemporalDateTimeString();
  if (dateTime.isOk()) {
    return dateTime;
  }
  likelyError.update(dateTime, reader_.index());

  // Restart parsing from the start of the string.
  reader_.reset();

  auto instant = parseTemporalInstantString();
  if (instant.isOk()) {
    return instant;
  }
  likelyError.update(instant, reader_.index());

  // Restart parsing from the start of the string.
  reader_.reset();

  auto time = parseTemporalTimeString();
  if (time.isOk()) {
    return time;
  }
  likelyError.update(time, reader_.index());

  // Restart parsing from the start of the string.
  reader_.reset();

  auto monthDay = parseTemporalMonthDayString();
  if (monthDay.isOk()) {
    return monthDay;
  }
  likelyError.update(monthDay, reader_.index());

  // Restart parsing from the start of the string.
  reader_.reset();

  auto yearMonth = parseTemporalYearMonthString();
  if (yearMonth.isOk()) {
    return yearMonth;
  }
  likelyError.update(yearMonth, reader_.index());

  return likelyError.propagate();
}

/**
 * ParseTemporalTimeZoneString ( timeZoneString )
 */
template <typename CharT>
static auto ParseTemporalTimeZoneString(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTemporalTimeZoneString();
}

/**
 * ParseTemporalTimeZoneString ( timeZoneString )
 */
static auto ParseTemporalTimeZoneString(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTemporalTimeZoneString<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseTemporalTimeZoneString<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseTemporalTimeZoneString ( timeZoneString )
 */
bool js::temporal::ParseTemporalTimeZoneString(
    JSContext* cx, Handle<JSString*> str,
    MutableHandle<ParsedTimeZone> result) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Steps 1-4.
  auto parseResult = ::ParseTemporalTimeZoneString(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "time zone");
    return false;
  }
  ZonedDateTimeString parsed = parseResult.unwrap();
  const auto& timeZone = parsed.timeZone;

  // Step 3.
  ISODateTime unused;
  if (!ParseISODateTime(cx, parsed, &unused)) {
    return false;
  }

  if (timeZone.hasAnnotation()) {
    // Case 1: 19700101T00:00Z[+02:00]
    // Case 2: 19700101T00:00+00:00[+02:00]
    // Case 3: 19700101T00:00[+02:00]
    // Case 4: 19700101T00:00Z[Europe/Berlin]
    // Case 5: 19700101T00:00+00:00[Europe/Berlin]
    // Case 6: 19700101T00:00[Europe/Berlin]

    if (!ParseTimeZoneAnnotation(cx, timeZone.annotation, linear, result)) {
      return false;
    }
  } else if (timeZone.isUTC()) {
    result.set(ParsedTimeZone::fromName(cx->names().UTC));
  } else if (timeZone.hasOffset()) {
    // ToTemporalTimeZoneSlotValue, step 7.
    //
    // Error reporting for sub-minute precision moved here.
    if (timeZone.offset.subMinutePrecision) {
      JS_ReportErrorNumberASCII(
          cx, GetErrorMessage, nullptr,
          JSMSG_TEMPORAL_PARSER_INVALID_SUBMINUTE_TIMEZONE, "time zone");
      return false;
    }

    int32_t offset = ParseTimeZoneOffset(timeZone.offset.toTimeZoneUTCOffset());
    result.set(ParsedTimeZone::fromOffset(offset));
  } else {
    // Step 5.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_PARSER_MISSING_TIMEZONE,
                              "time zone");
    return false;
  }

  // Step 6.
  return true;
}

template <typename CharT>
mozilla::Result<TimeZoneAnnotation, ParserError>
TemporalParser<CharT>::parseTimeZoneIdentifier() {
  MOZ_TRY(nonempty());
  return parse(timeZoneIdentifier());
}

/**
 * ParseTimeZoneIdentifier ( identifier )
 */
template <typename CharT>
static auto ParseTimeZoneIdentifier(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTimeZoneIdentifier();
}

/**
 * ParseTimeZoneIdentifier ( identifier )
 */
static auto ParseTimeZoneIdentifier(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTimeZoneIdentifier<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseTimeZoneIdentifier<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseTimeZoneIdentifier ( identifier )
 */
bool js::temporal::ParseTimeZoneIdentifier(
    JSContext* cx, Handle<JSString*> str,
    MutableHandle<ParsedTimeZone> result) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Steps 1-2.
  auto parseResult = ::ParseTimeZoneIdentifier(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "time zone identifier");
    return false;
  }
  auto timeZone = parseResult.unwrap();

  // Steps 3-4.
  return ParseTimeZoneAnnotation(cx, timeZone, linear, result);
}

template <typename CharT>
mozilla::Result<DateTimeUTCOffset, ParserError>
TemporalParser<CharT>::parseDateTimeUTCOffset() {
  MOZ_TRY(nonempty());
  return parse(utcOffsetSubMinutePrecision());
}

/**
 * ParseDateTimeUTCOffset ( offsetString )
 */
template <typename CharT>
static auto ParseDateTimeUTCOffset(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseDateTimeUTCOffset();
}

/**
 * ParseDateTimeUTCOffset ( offsetString )
 */
static auto ParseDateTimeUTCOffset(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseDateTimeUTCOffset<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseDateTimeUTCOffset<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseDateTimeUTCOffset ( offsetString )
 */
bool js::temporal::ParseDateTimeUTCOffset(JSContext* cx, Handle<JSString*> str,
                                          int64_t* result) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Steps 1-2.
  auto parseResult = ::ParseDateTimeUTCOffset(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "UTC offset");
    return false;
  }

  // Steps 3-21.
  *result = ParseDateTimeUTCOffset(parseResult.unwrap());
  return true;
}

template <typename CharT>
mozilla::Result<double, ParserError> TemporalParser<CharT>::durationDigits(
    JSContext* cx) {
  auto d = digits(cx);
  if (!d) {
    return err(JSMSG_TEMPORAL_PARSER_MISSING_DURATION_DIGITS);
  }
  return *d;
}

template <typename CharT>
mozilla::Result<TemporalDurationString, ParserError>
TemporalParser<CharT>::parseTemporalDurationString(JSContext* cx) {
  MOZ_TRY(nonempty());

  // Initialize all fields to zero.
  TemporalDurationString result{};

  // TemporalDurationString :::
  //   Duration
  //
  // Duration :::
  //   ASCIISign? DurationDesignator DurationDate
  //   ASCIISign? DurationDesignator DurationTime

  if (hasSign()) {
    result.sign = sign();
  }

  if (!durationDesignator()) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_DURATION_DESIGNATOR);
  }

  // DurationDate :::
  //   DurationYearsPart DurationTime?
  //   DurationMonthsPart DurationTime?
  //   DurationWeeksPart DurationTime?
  //   DurationDaysPart DurationTime?

  do {
    if (hasTimeDesignator()) {
      break;
    }

    double num;
    MOZ_TRY_VAR(num, durationDigits(cx));

    // DurationYearsPart :::
    //   DecimalDigits[~Sep] YearsDesignator DurationMonthsPart
    //   DecimalDigits[~Sep] YearsDesignator DurationWeeksPart
    //   DecimalDigits[~Sep] YearsDesignator DurationDaysPart?
    if (yearsDesignator()) {
      result.years = num;
      if (reader_.atEnd()) {
        return result;
      }
      if (hasTimeDesignator()) {
        break;
      }
      MOZ_TRY_VAR(num, durationDigits(cx));
    }

    // DurationMonthsPart :::
    //   DecimalDigits[~Sep] MonthsDesignator DurationWeeksPart
    //   DecimalDigits[~Sep] MonthsDesignator DurationDaysPart?
    if (monthsDesignator()) {
      result.months = num;
      if (reader_.atEnd()) {
        return result;
      }
      if (hasTimeDesignator()) {
        break;
      }
      MOZ_TRY_VAR(num, durationDigits(cx));
    }

    // DurationWeeksPart :::
    //   DecimalDigits[~Sep] WeeksDesignator DurationDaysPart?
    if (weeksDesignator()) {
      result.weeks = num;
      if (reader_.atEnd()) {
        return result;
      }
      if (hasTimeDesignator()) {
        break;
      }
      MOZ_TRY_VAR(num, durationDigits(cx));
    }

    // DurationDaysPart :::
    //   DecimalDigits[~Sep] DaysDesignator
    if (daysDesignator()) {
      result.days = num;
      if (reader_.atEnd()) {
        return result;
      }
      if (hasTimeDesignator()) {
        break;
      }
    }

    return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_DURATION_UNIT_DESIGNATOR);
  } while (false);

  // DurationTime :::
  //   TimeDesignator DurationHoursPart
  //   TimeDesignator DurationMinutesPart
  //   TimeDesignator DurationSecondsPart
  if (!timeDesignator()) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_TIME_DESIGNATOR);
  }

  double num;
  MOZ_TRY_VAR(num, durationDigits(cx));

  auto frac = fraction();

  // DurationHoursPart :::
  //   DecimalDigits[~Sep] TemporalDecimalFraction HoursDesignator
  //   DecimalDigits[~Sep] HoursDesignator DurationMinutesPart
  //   DecimalDigits[~Sep] HoursDesignator DurationSecondsPart?
  bool hasHoursFraction = false;
  if (hoursDesignator()) {
    hasHoursFraction = bool(frac);
    result.hours = num;
    result.hoursFraction = frac.valueOr(0);
    if (reader_.atEnd()) {
      return result;
    }

    MOZ_TRY_VAR(num, durationDigits(cx));
    frac = fraction();
  }

  // DurationMinutesPart :::
  //   DecimalDigits[~Sep] TemporalDecimalFraction MinutesDesignator
  //   DecimalDigits[~Sep] MinutesDesignator DurationSecondsPart?
  bool hasMinutesFraction = false;
  if (minutesDesignator()) {
    if (hasHoursFraction) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_DURATION_MINUTES);
    }
    hasMinutesFraction = bool(frac);
    result.minutes = num;
    result.minutesFraction = frac.valueOr(0);
    if (reader_.atEnd()) {
      return result;
    }

    MOZ_TRY_VAR(num, durationDigits(cx));
    frac = fraction();
  }

  // DurationSecondsPart :::
  //   DecimalDigits[~Sep] TemporalDecimalFraction? SecondsDesignator
  if (secondsDesignator()) {
    if (hasHoursFraction || hasMinutesFraction) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_DURATION_SECONDS);
    }
    result.seconds = num;
    result.secondsFraction = frac.valueOr(0);
    if (reader_.atEnd()) {
      return result;
    }
  }

  return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_DURATION_UNIT_DESIGNATOR);
}

/**
 * ParseTemporalDurationString ( isoString )
 */
template <typename CharT>
static auto ParseTemporalDurationString(JSContext* cx,
                                        mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTemporalDurationString(cx);
}

/**
 * ParseTemporalDurationString ( isoString )
 */
static auto ParseTemporalDurationString(JSContext* cx,
                                        Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTemporalDurationString<Latin1Char>(cx, str->latin1Range(nogc));
  }
  return ParseTemporalDurationString<char16_t>(cx, str->twoByteRange(nogc));
}

/**
 * ParseTemporalDurationString ( isoString )
 */
bool js::temporal::ParseTemporalDurationString(JSContext* cx,
                                               Handle<JSString*> str,
                                               Duration* result) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Steps 1-3.
  auto parseResult = ::ParseTemporalDurationString(cx, linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "duration");
    return false;
  }
  TemporalDurationString parsed = parseResult.unwrap();

  // Steps 4-8.
  double years = parsed.years;
  double months = parsed.months;
  double weeks = parsed.weeks;
  double days = parsed.days;
  double hours = parsed.hours;

  // Steps 9-17.
  double minutes, seconds, milliseconds, microseconds, nanoseconds;
  if (parsed.hoursFraction) {
    MOZ_ASSERT(parsed.hoursFraction > 0);
    MOZ_ASSERT(parsed.hoursFraction < 1'000'000'000);

    // Step 9.a.
    MOZ_ASSERT(parsed.minutes == 0);
    MOZ_ASSERT(parsed.minutesFraction == 0);
    MOZ_ASSERT(parsed.seconds == 0);
    MOZ_ASSERT(parsed.secondsFraction == 0);

    // Steps 9.b-d.
    int64_t h = int64_t(parsed.hoursFraction) * 60;
    minutes = double(h / 1'000'000'000);

    // Steps 13 and 15-17.
    int64_t min = (h % 1'000'000'000) * 60;
    seconds = double(min / 1'000'000'000);
    milliseconds = double((min % 1'000'000'000) / 1'000'000);
    microseconds = double((min % 1'000'000) / 1'000);
    nanoseconds = double(min % 1'000);
  }

  // Step 11.
  else if (parsed.minutesFraction) {
    MOZ_ASSERT(parsed.minutesFraction > 0);
    MOZ_ASSERT(parsed.minutesFraction < 1'000'000'000);

    // Step 11.a.
    MOZ_ASSERT(parsed.seconds == 0);
    MOZ_ASSERT(parsed.secondsFraction == 0);

    // Step 10.
    minutes = parsed.minutes;

    // Steps 11.b-d and 15-17.
    int64_t min = int64_t(parsed.minutesFraction) * 60;
    seconds = double(min / 1'000'000'000);
    milliseconds = double((min % 1'000'000'000) / 1'000'000);
    microseconds = double((min % 1'000'000) / 1'000);
    nanoseconds = double(min % 1'000);
  }

  // Step 14.
  else if (parsed.secondsFraction) {
    MOZ_ASSERT(parsed.secondsFraction > 0);
    MOZ_ASSERT(parsed.secondsFraction < 1'000'000'000);

    // Step 10.
    minutes = parsed.minutes;

    // Step 12.
    seconds = parsed.seconds;

    // Steps 14, 16-17
    milliseconds = double(parsed.secondsFraction / 1'000'000);
    microseconds = double((parsed.secondsFraction % 1'000'000) / 1'000);
    nanoseconds = double(parsed.secondsFraction % 1'000);
  } else {
    // Step 10.
    minutes = parsed.minutes;

    // Step 12.
    seconds = parsed.seconds;

    // Steps 15-17
    milliseconds = 0;
    microseconds = 0;
    nanoseconds = 0;
  }

  // Steps 18-19.
  int32_t factor = parsed.sign ? parsed.sign : 1;
  MOZ_ASSERT(factor == -1 || factor == 1);

  // Steps 20-29.
  *result = {
      (years * factor) + (+0.0),        (months * factor) + (+0.0),
      (weeks * factor) + (+0.0),        (days * factor) + (+0.0),
      (hours * factor) + (+0.0),        (minutes * factor) + (+0.0),
      (seconds * factor) + (+0.0),      (milliseconds * factor) + (+0.0),
      (microseconds * factor) + (+0.0), (nanoseconds * factor) + (+0.0),
  };

  // Steps 30-31.
  return ThrowIfInvalidDuration(cx, *result);
}

template <typename CharT>
mozilla::Result<AnnotationKey, ParserError>
TemporalParser<CharT>::annotationKey() {
  // AnnotationKey :::
  //    AKeyLeadingChar
  //    AnnotationKey AKeyChar

  size_t start = reader_.index();

  if (!aKeyLeadingChar()) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_ANNOTATION_KEY);
  }

  // Optionally followed by a sequence of |AKeyChar|.
  while (aKeyChar()) {
  }

  return AnnotationKey{start, reader_.index() - start};
}

template <typename CharT>
mozilla::Result<AnnotationValue, ParserError>
TemporalParser<CharT>::annotationValue() {
  // AnnotationValue :::
  //   AnnotationValueComponent
  //   AnnotationValueComponent - AnnotationValue

  size_t start = reader_.index();

  do {
    if (!annotationValueComponent()) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_ANNOTATION_VALUE);
    }
  } while (character('-'));

  return AnnotationValue{start, reader_.index() - start};
}

template <typename CharT>
mozilla::Result<Annotation, ParserError> TemporalParser<CharT>::annotation() {
  // Annotation :::
  //   [ AnnotationCriticalFlag? AnnotationKey = AnnotationValue ]

  if (!character('[')) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_BRACKET_BEFORE_ANNOTATION);
  }

  Annotation result{};

  result.critical = annotationCriticalFlag();

  MOZ_TRY_VAR(result.key, annotationKey());

  if (!character('=')) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_ASSIGNMENT_IN_ANNOTATION);
  }

  MOZ_TRY_VAR(result.value, annotationValue());

  if (!character(']')) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_BRACKET_AFTER_ANNOTATION);
  }

  return result;
}

template <typename CharT>
mozilla::Result<CalendarName, ParserError>
TemporalParser<CharT>::annotations() {
  // Annotations :::
  //   Annotation Annotations?

  MOZ_ASSERT(hasAnnotationStart());

  CalendarName calendar;
  bool calendarWasCritical = false;
  while (hasAnnotationStart()) {
    Annotation anno;
    MOZ_TRY_VAR(anno, annotation());

    auto [key, value, critical] = anno;

    static constexpr std::string_view ca = "u-ca";

    auto keySpan = reader_.substring(key);
    if (keySpan.size() == ca.length() &&
        std::equal(ca.begin(), ca.end(), keySpan.data())) {
      if (!calendar.present()) {
        calendar = value;
        calendarWasCritical = critical;
      } else if (critical || calendarWasCritical) {
        return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_CRITICAL_ANNOTATION);
      }
    } else if (critical) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_INVALID_CRITICAL_ANNOTATION);
    }
  }
  return calendar;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::annotatedTime() {
  // clang-format off
  //
  // AnnotatedTime :::
  //   TimeDesignator Time DateTimeUTCOffset[~Z]? TimeZoneAnnotation? Annotations?
  //   Time DateTimeUTCOffset[~Z]? TimeZoneAnnotation? Annotations?
  //
  // clang-format on

  ZonedDateTimeString result{};

  size_t start = reader_.index();
  bool hasTimeDesignator = timeDesignator();

  MOZ_TRY_VAR(result.time, time());

  if (hasDateTimeUTCOffsetStart()) {
    MOZ_TRY_VAR(result.timeZone, dateTimeUTCOffset(/* allowZ = */ false));
  }

  // Early error if `Time DateTimeUTCOffset[~Z]` can be parsed as either
  // `DateSpecMonthDay` or `DateSpecYearMonth`.
  if (!hasTimeDesignator) {
    size_t end = reader_.index();

    auto isValidMonthDay = [](const ISODate& date) {
      MOZ_ASSERT(date.year == AbsentYear);
      MOZ_ASSERT(1 <= date.month && date.month <= 12);
      MOZ_ASSERT(1 <= date.day && date.day <= 31);

      constexpr int32_t leapYear = 0;
      return date.day <= ISODaysInMonth(leapYear, date.month);
    };

    // Reset and check if the input can also be parsed as DateSpecMonthDay.
    reader_.reset(start);

    if (auto monthDay = dateSpecMonthDay(); monthDay.isOk()) {
      if (reader_.index() == end && isValidMonthDay(monthDay.unwrap())) {
        return mozilla::Err(JSMSG_TEMPORAL_PARSER_AMBIGUOUS_TIME_MONTH_DAY);
      }
    }

    // Reset and check if the input can also be parsed as DateSpecYearMonth.
    reader_.reset(start);

    if (dateSpecYearMonth().isOk()) {
      if (reader_.index() == end) {
        return mozilla::Err(JSMSG_TEMPORAL_PARSER_AMBIGUOUS_TIME_YEAR_MONTH);
      }
    }

    // Input can neither be parsed as DateSpecMonthDay nor DateSpecYearMonth.
    reader_.reset(end);
  }

  if (hasTimeZoneAnnotationStart()) {
    MOZ_TRY_VAR(result.timeZone.annotation, timeZoneAnnotation());
  }

  if (hasAnnotationStart()) {
    MOZ_TRY_VAR(result.calendar, annotations());
  }

  return result;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::annotatedDateTime() {
  // AnnotatedDateTime[Zoned, TimeRequired] :::
  //  [~Zoned] DateTime[~Z, ?TimeRequired] TimeZoneAnnotation? Annotations?
  //  [+Zoned] DateTime[+Z, ?TimeRequired] TimeZoneAnnotation Annotations?
  //
  // When called as `AnnotatedDateTime[~Zoned, ~TimeRequired]`.

  ZonedDateTimeString result;
  MOZ_TRY_VAR(result, dateTime(/* allowZ = */ false));

  if (hasTimeZoneAnnotationStart()) {
    MOZ_TRY_VAR(result.timeZone.annotation, timeZoneAnnotation());
  }

  if (hasAnnotationStart()) {
    MOZ_TRY_VAR(result.calendar, annotations());
  }

  return result;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::annotatedDateTimeTimeRequired() {
  // AnnotatedDateTime[Zoned, TimeRequired] :::
  //  [~Zoned] DateTime[~Z, ?TimeRequired] TimeZoneAnnotation? Annotations?
  //  [+Zoned] DateTime[+Z, ?TimeRequired] TimeZoneAnnotation Annotations?
  //
  // DateTime[Z, TimeRequired] :::
  //   [~TimeRequired] Date
  //   Date DateTimeSeparator Time DateTimeUTCOffset[?Z]?
  //
  // When called as `AnnotatedDateTime[~Zoned, +TimeRequired]`.

  ZonedDateTimeString result{};

  MOZ_TRY_VAR(result.date, date());

  if (!dateTimeSeparator()) {
    return mozilla::Err(JSMSG_TEMPORAL_PARSER_MISSING_DATE_TIME_SEPARATOR);
  }

  MOZ_TRY_VAR(result.time, time());

  if (hasDateTimeUTCOffsetStart()) {
    MOZ_TRY_VAR(result.timeZone, dateTimeUTCOffset(/* allowZ = */ false));
  }

  if (hasTimeZoneAnnotationStart()) {
    MOZ_TRY_VAR(result.timeZone.annotation, timeZoneAnnotation());
  }

  if (hasAnnotationStart()) {
    MOZ_TRY_VAR(result.calendar, annotations());
  }

  return result;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::annotatedYearMonth() {
  // AnnotatedYearMonth :::
  //   DateSpecYearMonth TimeZoneAnnotation? Annotations?

  ZonedDateTimeString result{};

  MOZ_TRY_VAR(result.date, dateSpecYearMonth());

  if (hasTimeZoneAnnotationStart()) {
    MOZ_TRY_VAR(result.timeZone.annotation, timeZoneAnnotation());
  }

  if (hasAnnotationStart()) {
    MOZ_TRY_VAR(result.calendar, annotations());
  }

  return result;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::annotatedMonthDay() {
  // AnnotatedMonthDay :::
  //   DateSpecMonthDay TimeZoneAnnotation? Annotations?

  ZonedDateTimeString result{};

  MOZ_TRY_VAR(result.date, dateSpecMonthDay());

  if (hasTimeZoneAnnotationStart()) {
    MOZ_TRY_VAR(result.timeZone.annotation, timeZoneAnnotation());
  }

  if (hasAnnotationStart()) {
    MOZ_TRY_VAR(result.calendar, annotations());
  }

  return result;
}

template <typename CharT>
mozilla::Result<ISODate, ParserError>
TemporalParser<CharT>::dateSpecYearMonth() {
  // DateSpecYearMonth :::
  //   DateYear DateSeparator[+Extended] DateMonth
  //   DateYear DateSeparator[~Extended] DateMonth

  ISODate result{};

  MOZ_TRY_VAR(result.year, dateYear());

  // Optional |DateSeparator|.
  dateSeparator();

  MOZ_TRY_VAR(result.month, dateMonth());

  return result;
}

template <typename CharT>
mozilla::Result<ISODate, ParserError>
TemporalParser<CharT>::dateSpecMonthDay() {
  // DateSpecMonthDay :::
  //   --? DateMonth DateSeparator[+Extended] DateDay
  //   --? DateMonth DateSeparator[~Extended] DateDay

  ISODate result{};

  // Optional: --
  string("--");

  result.year = AbsentYear;

  MOZ_TRY_VAR(result.month, dateMonth());

  // Optional |DateSeparator|.
  dateSeparator();

  MOZ_TRY_VAR(result.day, dateDay());

  return result;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::parseTemporalCalendarString() {
  MOZ_TRY(nonempty());

  // Handle the common case of a standalone calendar name first.
  //
  // All valid calendar names start with two alphabetic characters and none of
  // the ParseISODateTime parse goals can start with two alphabetic characters.
  // TemporalTimeString can start with 'T', so we can't only check the first
  // character.
  if (hasTwoAsciiAlpha()) {
    ZonedDateTimeString result{};

    MOZ_TRY_VAR(result.calendar, parse(annotationValue()));

    return result;
  }

  LikelyError likelyError{};

  // Try all five parse goals from ParseISODateTime in order.
  //
  // TemporalDateTimeString
  // TemporalInstantString
  // TemporalTimeString
  // TemporalMonthDayString
  // TemporalYearMonthString

  auto dateTime = parseTemporalDateTimeString();
  if (dateTime.isOk()) {
    return dateTime;
  }
  likelyError.update(dateTime, reader_.index());

  // Restart parsing from the start of the string.
  reader_.reset();

  auto instant = parseTemporalInstantString();
  if (instant.isOk()) {
    return instant;
  }
  likelyError.update(instant, reader_.index());

  // Restart parsing from the start of the string.
  reader_.reset();

  auto time = parseTemporalTimeString();
  if (time.isOk()) {
    return time;
  }
  likelyError.update(time, reader_.index());

  // Restart parsing from the start of the string.
  reader_.reset();

  auto monthDay = parseTemporalMonthDayString();
  if (monthDay.isOk()) {
    return monthDay;
  }
  likelyError.update(monthDay, reader_.index());

  // Restart parsing from the start of the string.
  reader_.reset();

  auto yearMonth = parseTemporalYearMonthString();
  if (yearMonth.isOk()) {
    return yearMonth;
  }
  likelyError.update(yearMonth, reader_.index());

  return likelyError.propagate();
}

/**
 * ParseTemporalCalendarString ( isoString )
 */
template <typename CharT>
static auto ParseTemporalCalendarString(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTemporalCalendarString();
}

/**
 * ParseTemporalCalendarString ( isoString )
 */
static auto ParseTemporalCalendarString(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTemporalCalendarString<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseTemporalCalendarString<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseTemporalCalendarString ( isoString )
 */
JSLinearString* js::temporal::ParseTemporalCalendarString(
    JSContext* cx, Handle<JSString*> str) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return nullptr;
  }

  // Steps 1 and 3.a.
  auto parseResult = ::ParseTemporalCalendarString(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "calendar");
    return nullptr;
  }
  ZonedDateTimeString parsed = parseResult.unwrap();

  ISODateTime unused;
  if (!ParseISODateTime(cx, parsed, &unused)) {
    return nullptr;
  }

  // Step 2.b.
  if (!parsed.calendar.present()) {
    return cx->names().iso8601;
  }

  // Steps 2.c and 3.c
  return ToString(cx, linear, parsed.calendar);
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::parseTemporalTimeString() {
  MOZ_TRY(nonempty());

  // TemporalTimeString :::
  //   AnnotatedTime
  //   AnnotatedDateTime[~Zoned, +TimeRequired]

  LikelyError likelyError{};

  auto time = parse(annotatedTime());
  if (time.isOk()) {
    return time;
  }
  likelyError.update(time, reader_.index());

  // Reset and try the next option.
  reader_.reset();

  auto dateTime = parse(annotatedDateTimeTimeRequired());
  if (dateTime.isOk()) {
    return dateTime;
  }
  likelyError.update(time, reader_.index());

  // Set current index to the likely error index to give better error messages
  // when called from parserTemporal{Calendar,TimeZone}String.
  reader_.reset(likelyError.index());

  return likelyError.propagate();
}

/**
 * ParseTemporalTimeString ( isoString )
 */
template <typename CharT>
static auto ParseTemporalTimeString(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTemporalTimeString();
}

/**
 * ParseTemporalTimeString ( isoString )
 */
static auto ParseTemporalTimeString(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTemporalTimeString<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseTemporalTimeString<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseTemporalTimeString ( isoString )
 */
bool js::temporal::ParseTemporalTimeString(JSContext* cx, Handle<JSString*> str,
                                           Time* result) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Steps 1-2.
  auto parseResult = ::ParseTemporalTimeString(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "time");
    return false;
  }
  ZonedDateTimeString parsed = parseResult.unwrap();

  // Step 3.
  ISODateTime dateTime;
  if (!ParseISODateTime(cx, parsed, &dateTime)) {
    return false;
  }
  *result = dateTime.time;

  // Step 4.
  MOZ_ASSERT(!parsed.startOfDay);

  // Step 5.
  return true;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::parseTemporalMonthDayString() {
  MOZ_TRY(nonempty());

  // TemporalMonthDayString :::
  //   AnnotatedMonthDay
  //   AnnotatedDateTime[~Zoned, ~TimeRequired]

  LikelyError likelyError{};

  auto monthDay = parse(annotatedMonthDay());
  if (monthDay.isOk()) {
    auto result = monthDay.unwrap();

    // ParseISODateTime, step 3.
    if (result.calendar.present() &&
        !IsISO8601Calendar(reader_.substring(result.calendar))) {
      return mozilla::Err(JSMSG_TEMPORAL_PARSER_MONTH_DAY_CALENDAR_NOT_ISO8601);
    }
    return result;
  }
  likelyError.update(monthDay, reader_.index());

  // Reset and try the next option.
  reader_.reset();

  auto dateTime = parse(annotatedDateTime());
  if (dateTime.isOk()) {
    return dateTime;
  }
  likelyError.update(dateTime, reader_.index());

  // Set current index to the likely error index to give better error messages
  // when called from parserTemporal{Calendar,TimeZone}String.
  reader_.reset(likelyError.index());

  return likelyError.propagate();
}

/**
 * ParseTemporalMonthDayString ( isoString )
 */
template <typename CharT>
static auto ParseTemporalMonthDayString(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTemporalMonthDayString();
}

/**
 * ParseTemporalMonthDayString ( isoString )
 */
static auto ParseTemporalMonthDayString(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTemporalMonthDayString<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseTemporalMonthDayString<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseTemporalMonthDayString ( isoString )
 */
bool js::temporal::ParseTemporalMonthDayString(
    JSContext* cx, Handle<JSString*> str, ISODate* result, bool* hasYear,
    MutableHandle<JSString*> calendar) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Steps 1-2.
  auto parseResult = ::ParseTemporalMonthDayString(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "month-day");
    return false;
  }
  ZonedDateTimeString parsed = parseResult.unwrap();

  // Step 3.
  ISODateTime dateTime;
  if (!ParseISODateTime(cx, parsed, &dateTime)) {
    return false;
  }
  *result = dateTime.date;

  // Steps 4-5.
  *hasYear = parsed.date.year != AbsentYear;

  if (parsed.calendar.present()) {
    calendar.set(ToString(cx, linear, parsed.calendar));
    if (!calendar) {
      return false;
    }
  }

  // Step 6.
  return true;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::parseTemporalYearMonthString() {
  MOZ_TRY(nonempty());

  // TemporalYearMonthString :::
  //   AnnotatedYearMonth
  //   AnnotatedDateTime[~Zoned, ~TimeRequired]

  LikelyError likelyError{};

  auto yearMonth = parse(annotatedYearMonth());
  if (yearMonth.isOk()) {
    auto result = yearMonth.unwrap();

    // ParseISODateTime, step 3.
    if (result.calendar.present() &&
        !IsISO8601Calendar(reader_.substring(result.calendar))) {
      return mozilla::Err(
          JSMSG_TEMPORAL_PARSER_YEAR_MONTH_CALENDAR_NOT_ISO8601);
    }
    return result;
  }
  likelyError.update(yearMonth, reader_.index());

  // Reset and try the next option.
  reader_.reset();

  auto dateTime = parse(annotatedDateTime());
  if (dateTime.isOk()) {
    return dateTime;
  }
  likelyError.update(dateTime, reader_.index());

  // Set current index to the likely error index to give better error messages
  // when called from parserTemporal{Calendar,TimeZone}String.
  reader_.reset(likelyError.index());

  return likelyError.propagate();
}

/**
 * ParseTemporalYearMonthString ( isoString )
 */
template <typename CharT>
static auto ParseTemporalYearMonthString(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTemporalYearMonthString();
}

/**
 * ParseTemporalYearMonthString ( isoString )
 */
static auto ParseTemporalYearMonthString(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTemporalYearMonthString<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseTemporalYearMonthString<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseTemporalYearMonthString ( isoString )
 */
bool js::temporal::ParseTemporalYearMonthString(
    JSContext* cx, Handle<JSString*> str, ISODate* result,
    MutableHandle<JSString*> calendar) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Steps 1-2.
  auto parseResult = ::ParseTemporalYearMonthString(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "year-month");
    return false;
  }
  ZonedDateTimeString parsed = parseResult.unwrap();

  // Step 3.
  ISODateTime dateTime;
  if (!ParseISODateTime(cx, parsed, &dateTime)) {
    return false;
  }
  *result = dateTime.date;

  if (parsed.calendar.present()) {
    calendar.set(ToString(cx, linear, parsed.calendar));
    if (!calendar) {
      return false;
    }
  }

  return true;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::parseTemporalDateTimeString() {
  MOZ_TRY(nonempty());

  // TemporalDateTimeString[Zoned] :::
  //   AnnotatedDateTime[?Zoned, ~TimeRequired]

  return parse(annotatedDateTime());
}

/**
 * ParseTemporalDateTimeString ( isoString )
 */
template <typename CharT>
static auto ParseTemporalDateTimeString(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTemporalDateTimeString();
}

/**
 * ParseTemporalDateTimeString ( isoString )
 */
static auto ParseTemporalDateTimeString(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTemporalDateTimeString<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseTemporalDateTimeString<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseTemporalDateTimeString ( isoString )
 */
bool js::temporal::ParseTemporalDateTimeString(
    JSContext* cx, Handle<JSString*> str, ISODateTime* result,
    MutableHandle<JSString*> calendar) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Steps 1-2.
  auto parseResult = ::ParseTemporalDateTimeString(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "date-time");
    return false;
  }
  ZonedDateTimeString parsed = parseResult.unwrap();

  // Step 3.
  if (!ParseISODateTime(cx, parsed, result)) {
    return false;
  }

  if (parsed.calendar.present()) {
    calendar.set(ToString(cx, linear, parsed.calendar));
    if (!calendar) {
      return false;
    }
  }

  return true;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::parseTemporalZonedDateTimeString() {
  MOZ_TRY(nonempty());

  // Parse goal: TemporalDateTimeString[+Zoned]
  //
  // TemporalDateTimeString[Zoned] :::
  //   AnnotatedDateTime[?Zoned, ~TimeRequired]
  //
  // AnnotatedDateTime[Zoned, TimeRequired] :::
  //   [~Zoned] DateTime[~Z, ?TimeRequired] TimeZoneAnnotation? Annotations?
  //   [+Zoned] DateTime[+Z, ?TimeRequired] TimeZoneAnnotation Annotations?

  ZonedDateTimeString result{};

  MOZ_TRY_VAR(result, dateTime(/* allowZ = */ true));

  MOZ_TRY_VAR(result.timeZone.annotation, timeZoneAnnotation());

  if (hasAnnotationStart()) {
    MOZ_TRY_VAR(result.calendar, annotations());
  }

  return complete(result);
}

/**
 * ParseTemporalZonedDateTimeString ( isoString )
 */
template <typename CharT>
static auto ParseTemporalZonedDateTimeString(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTemporalZonedDateTimeString();
}

/**
 * ParseTemporalZonedDateTimeString ( isoString )
 */
static auto ParseTemporalZonedDateTimeString(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTemporalZonedDateTimeString<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseTemporalZonedDateTimeString<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseTemporalZonedDateTimeString ( isoString )
 */
bool js::temporal::ParseTemporalZonedDateTimeString(
    JSContext* cx, Handle<JSString*> str,
    JS::MutableHandle<ParsedZonedDateTime> result) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Step 1.
  auto parseResult = ::ParseTemporalZonedDateTimeString(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "zoned date-time");
    return false;
  }
  ZonedDateTimeString parsed = parseResult.unwrap();

  // Step 2. (ParseISODateTime, steps 2-3.)
  Rooted<JSLinearString*> calendar(cx);
  if (parsed.calendar.present()) {
    calendar = ToString(cx, linear, parsed.calendar);
    if (!calendar) {
      return false;
    }
  }

  // Step 2. (ParseISODateTime, steps 4-21.)
  ISODateTime dateTime;
  if (!ParseISODateTime(cx, parsed, &dateTime)) {
    return false;
  }

  // Step 2. (ParseISODateTime, steps 22-23.)
  bool isStartOfDay = parsed.startOfDay;

  // Step 2. (ParseISODateTime, steps 24-27.)
  bool isUTC;
  bool hasOffset;
  int64_t timeZoneOffset;
  Rooted<ParsedTimeZone> timeZoneAnnotation(cx);
  {
    MOZ_ASSERT(parsed.timeZone.hasAnnotation());

    // Case 1: 19700101T00:00Z[+02:00]
    // { [[Z]]: true, [[OffsetString]]: undefined, [[Name]]: "+02:00" }
    //
    // Case 2: 19700101T00:00+02:00[+02:00]
    // { [[Z]]: false, [[OffsetString]]: "+02:00", [[Name]]: "+02:00" }
    //
    // Case 3: 19700101[+02:00]
    // { [[Z]]: false, [[OffsetString]]: undefined, [[Name]]: "+02:00" }
    //
    // Case 4: 19700101T00:00Z[Europe/Berlin]
    // { [[Z]]: true, [[OffsetString]]: undefined, [[Name]]: "Europe/Berlin" }
    //
    // Case 5: 19700101T00:00+01:00[Europe/Berlin]
    // { [[Z]]: false, [[OffsetString]]: "+01:00", [[Name]]: "Europe/Berlin" }
    //
    // Case 6: 19700101[Europe/Berlin]
    // { [[Z]]: false, [[OffsetString]]: undefined, [[Name]]: "Europe/Berlin" }

    const auto& annotation = parsed.timeZone.annotation;
    if (!ParseTimeZoneAnnotation(cx, annotation, linear, &timeZoneAnnotation)) {
      return false;
    }

    if (parsed.timeZone.isUTC()) {
      isUTC = true;
      hasOffset = false;
      timeZoneOffset = 0;
    } else if (parsed.timeZone.hasOffset()) {
      isUTC = false;
      hasOffset = true;
      timeZoneOffset = ParseDateTimeUTCOffset(parsed.timeZone.offset);
    } else {
      isUTC = false;
      hasOffset = false;
      timeZoneOffset = 0;
    }
  }

  // Step 2. (ParseISODateTime, step 28.)
  result.set(ParsedZonedDateTime{
      dateTime,
      calendar,
      timeZoneAnnotation.get(),
      timeZoneOffset,
      isUTC,
      hasOffset,
      isStartOfDay,
  });
  return true;
}

template <typename CharT>
mozilla::Result<ZonedDateTimeString, ParserError>
TemporalParser<CharT>::parseTemporalRelativeToString() {
  MOZ_TRY(nonempty());

  // Parse goals:
  // TemporalDateTimeString[+Zoned] and TemporalDateTimeString[~Zoned]
  //
  // TemporalDateTimeString[Zoned] :::
  //   AnnotatedDateTime[?Zoned, ~TimeRequired]
  //
  // AnnotatedDateTime[Zoned, TimeRequired] :::
  //   [~Zoned] DateTime[~Z, ?TimeRequired] TimeZoneAnnotation? Annotations?
  //   [+Zoned] DateTime[+Z, ?TimeRequired] TimeZoneAnnotation Annotations?

  ZonedDateTimeString result{};

  MOZ_TRY_VAR(result, dateTime(/* allowZ = */ true));

  if (hasTimeZoneAnnotationStart()) {
    MOZ_TRY_VAR(result.timeZone.annotation, timeZoneAnnotation());
  }

  if (hasAnnotationStart()) {
    MOZ_TRY_VAR(result.calendar, annotations());
  }

  return complete(result);
}

/**
 * ParseTemporalRelativeToString ( isoString )
 */
template <typename CharT>
static auto ParseTemporalRelativeToString(mozilla::Span<const CharT> str) {
  TemporalParser<CharT> parser(str);
  return parser.parseTemporalRelativeToString();
}

/**
 * ParseTemporalRelativeToString ( isoString )
 */
static auto ParseTemporalRelativeToString(Handle<JSLinearString*> str) {
  JS::AutoCheckCannotGC nogc;
  if (str->hasLatin1Chars()) {
    return ParseTemporalRelativeToString<Latin1Char>(str->latin1Range(nogc));
  }
  return ParseTemporalRelativeToString<char16_t>(str->twoByteRange(nogc));
}

/**
 * ParseTemporalRelativeToString ( isoString )
 */
bool js::temporal::ParseTemporalRelativeToString(
    JSContext* cx, Handle<JSString*> str,
    MutableHandle<ParsedZonedDateTime> result) {
  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  // Steps 1-2.
  auto parseResult = ::ParseTemporalRelativeToString(linear);
  if (parseResult.isErr()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              parseResult.unwrapErr(), "relative date-time");
    return false;
  }
  ZonedDateTimeString parsed = parseResult.unwrap();

  // Step 3.
  if (parsed.timeZone.isUTC() && !parsed.timeZone.hasAnnotation()) {
    JS_ReportErrorNumberASCII(
        cx, GetErrorMessage, nullptr,
        JSMSG_TEMPORAL_PARSER_INVALID_UTC_DESIGNATOR_WITHOUT_NAME,
        "relative date-time");
    return false;
  }

  // Step 4. (ParseISODateTime, steps 1-18.)
  ISODateTime dateTime;
  if (!ParseISODateTime(cx, parsed, &dateTime)) {
    return false;
  }
  bool isStartOfDay = parsed.startOfDay;

  // Step 4. (ParseISODateTime, steps 19-22.)
  bool isUTC;
  bool hasOffset;
  int64_t timeZoneOffset;
  Rooted<ParsedTimeZone> timeZoneAnnotation(cx);
  if (parsed.timeZone.hasAnnotation()) {
    // Case 1: 19700101Z[+02:00]
    // { [[Z]]: true, [[OffsetString]]: undefined, [[Name]]: "+02:00" }
    //
    // Case 2: 19700101+00:00[+02:00]
    // { [[Z]]: false, [[OffsetString]]: "+00:00", [[Name]]: "+02:00" }
    //
    // Case 3: 19700101[+02:00]
    // { [[Z]]: false, [[OffsetString]]: undefined, [[Name]]: "+02:00" }
    //
    // Case 4: 19700101Z[Europe/Berlin]
    // { [[Z]]: true, [[OffsetString]]: undefined, [[Name]]: "Europe/Berlin" }
    //
    // Case 5: 19700101+00:00[Europe/Berlin]
    // { [[Z]]: false, [[OffsetString]]: "+00:00", [[Name]]: "Europe/Berlin" }
    //
    // Case 6: 19700101[Europe/Berlin]
    // { [[Z]]: false, [[OffsetString]]: undefined, [[Name]]: "Europe/Berlin" }

    const auto& annotation = parsed.timeZone.annotation;
    if (!ParseTimeZoneAnnotation(cx, annotation, linear, &timeZoneAnnotation)) {
      return false;
    }

    if (parsed.timeZone.isUTC()) {
      isUTC = true;
      hasOffset = false;
      timeZoneOffset = 0;
    } else if (parsed.timeZone.hasOffset()) {
      isUTC = false;
      hasOffset = true;
      timeZoneOffset = ParseDateTimeUTCOffset(parsed.timeZone.offset);
    } else {
      isUTC = false;
      hasOffset = false;
      timeZoneOffset = 0;
    }
  } else {
    // GetTemporalRelativeToOption ignores any other time zone information when
    // no bracketed time zone annotation is present.

    isUTC = false;
    hasOffset = false;
    timeZoneOffset = 0;
    timeZoneAnnotation.set(ParsedTimeZone{});
  }

  // Step 4. (ParseISODateTime, steps 23-24.)
  JSLinearString* calendar = nullptr;
  if (parsed.calendar.present()) {
    calendar = ToString(cx, linear, parsed.calendar);
    if (!calendar) {
      return false;
    }
  }

  // Step 4. (Return)
  result.set(ParsedZonedDateTime{
      dateTime,
      calendar,
      timeZoneAnnotation.get(),
      timeZoneOffset,
      isUTC,
      hasOffset,
      isStartOfDay,
  });
  return true;
}

void js::temporal::ParsedTimeZone::trace(JSTracer* trc) {
  TraceNullableRoot(trc, &name, "ParsedTimeZone::name");
}

void js::temporal::ParsedZonedDateTime::trace(JSTracer* trc) {
  TraceNullableRoot(trc, &calendar, "ParsedZonedDateTime::calendar");
  timeZoneAnnotation.trace(trc);
}
