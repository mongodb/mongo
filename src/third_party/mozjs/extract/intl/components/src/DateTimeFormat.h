/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef intl_components_DateTimeFormat_h_
#define intl_components_DateTimeFormat_h_
#include "unicode/udat.h"

#include "mozilla/Assertions.h"
#include "mozilla/intl/ICU4CGlue.h"
#include "mozilla/Maybe.h"
#include "mozilla/Result.h"
#include "mozilla/ResultVariant.h"
#include "mozilla/Span.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Utf8.h"
#include "mozilla/Vector.h"

namespace mozilla::intl {

enum class DateTimeStyle { Full, Long, Medium, Short, None };

class Calendar;

/**
 * This component is a Mozilla-focused API for the date formatting provided by
 * ICU. The methods internally call out to ICU4C. This is responsible for and
 * owns any resources opened through ICU, through RAII.
 *
 * The construction of a DateTimeFormat contains the majority of the cost
 * of the DateTimeFormat operation. DateTimeFormat::TryFormat should be
 * relatively inexpensive after the initial construction.
 *
 * This class supports creating from Styles (a fixed set of options), and from
 * Skeletons (a list of fields and field widths to include).
 *
 * This API will also serve to back the ECMA-402 Intl.DateTimeFormat API.
 * See Bug 1709473.
 * https://tc39.es/ecma402/#datetimeformat-objects
 */
class DateTimeFormat final {
 public:
  // Do not allow copy as this class owns the ICU resource. Move is not
  // currently implemented, but a custom move operator could be created if
  // needed.
  DateTimeFormat(const DateTimeFormat&) = delete;
  DateTimeFormat& operator=(const DateTimeFormat&) = delete;

  enum class StyleError { DateFormatFailure };

  /**
   * Create a DateTimeFormat from styles.
   *
   * The "style" model uses different options for formatting a date or time
   * based on how the result will be styled, rather than picking specific
   * fields or lengths.
   *
   * Takes an optional time zone which will override the user's default
   * time zone. This is a UTF-16 string that takes the form "GMT±hh:mm", or
   * an IANA time zone identifier, e.g. "America/Chicago".
   */
  static Result<UniquePtr<DateTimeFormat>, DateTimeFormat::StyleError>
  TryCreateFromStyle(Span<const char> aLocale, DateTimeStyle aDateStyle,
                     DateTimeStyle aTimeStyle,
                     Maybe<Span<const char16_t>> aTimeZoneOverride = Nothing{});

  enum class SkeletonError {
    OutOfMemory,
    PatternGeneratorFailure,
    GetBestPatternFailure,
    DateFormatFailure
  };

  /**
   * Create a DateTimeFormat from a UTF-8 skeleton. See the UTF-16 version for
   * the full documentation of this function. This overload requires additional
   * work compared to the UTF-16 version.
   */
  static Result<UniquePtr<DateTimeFormat>, DateTimeFormat::SkeletonError>
  TryCreateFromSkeleton(Span<const char> aLocale, Span<const char> aSkeleton,
                        Maybe<Span<const char>> aTimeZoneOverride = Nothing{});

  /**
   * Create a DateTimeFormat from a UTF-16 skeleton.
   *
   * A skeleton is an unordered list of fields that are used to find an
   * appropriate date time format pattern. Example skeletons would be "yMd",
   * "yMMMd", "EBhm". If the skeleton includes string literals or other
   * information, it will be discarded when matching against skeletons.
   *
   * Takes an optional time zone which will override the user's default
   * time zone. This is a string that takes the form "GMT±hh:mm", or
   * an IANA time zone identifier, e.g. "America/Chicago".
   */
  static Result<UniquePtr<DateTimeFormat>, DateTimeFormat::SkeletonError>
  TryCreateFromSkeleton(
      Span<const char> aLocale, Span<const char16_t> aSkeleton,
      Maybe<Span<const char16_t>> aTimeZoneOverride = Nothing{});

  enum class PatternError { DateFormatFailure };

  static Result<UniquePtr<DateTimeFormat>, DateTimeFormat::PatternError>
  TryCreateFromPattern(
      Span<const char> aLocale, Span<const char16_t> aPattern,
      Maybe<Span<const char16_t>> aTimeZoneOverride = Nothing{});

  /**
   * Use the format settings to format a date time into a string. The non-null
   * terminated string will be placed into the provided buffer. The idea behind
   * this API is that the constructor is expensive, and then the format
   * operation is cheap.
   *
   * aUnixEpoch is the number of milliseconds since 1 January 1970, UTC.
   */
  template <typename B>
  ICUResult TryFormat(double aUnixEpoch, B& aBuffer) const {
    static_assert(
        std::is_same<typename B::CharType, unsigned char>::value ||
            std::is_same<typename B::CharType, char>::value ||
            std::is_same<typename B::CharType, char16_t>::value,
        "The only buffer CharTypes supported by DateTimeFormat are char "
        "(for UTF-8 support) and char16_t (for UTF-16 support).");

    if constexpr (std::is_same<typename B::CharType, char>::value ||
                  std::is_same<typename B::CharType, unsigned char>::value) {
      // The output buffer is UTF-8, but ICU uses UTF-16 internally.

      // Write the formatted date into the u16Buffer.
      mozilla::Vector<char16_t, StackU16VectorSize> u16Vec;

      auto result = FillVectorWithICUCall(
          u16Vec, [this, &aUnixEpoch](UChar* target, int32_t length,
                                      UErrorCode* status) {
            return udat_format(mDateFormat, aUnixEpoch, target, length,
                               /* UFieldPosition* */ nullptr, status);
          });
      if (result.isErr()) {
        return result;
      }

      if (!FillUTF8Buffer(u16Vec, aBuffer)) {
        return Err(ICUError::OutOfMemory);
      }
      return Ok{};
    } else {
      static_assert(std::is_same<typename B::CharType, char16_t>::value);

      // The output buffer is UTF-16. ICU can output directly into this buffer.
      return FillBufferWithICUCall(
          aBuffer, [&](UChar* target, int32_t length, UErrorCode* status) {
            return udat_format(mDateFormat, aUnixEpoch, target, length, nullptr,
                               status);
          });
    }
  };

  /**
   * Copies the pattern for the current DateTimeFormat to a buffer.
   */
  template <typename B>
  ICUResult GetPattern(B& aBuffer) const {
    return FillBufferWithICUCall(
        aBuffer, [&](UChar* target, int32_t length, UErrorCode* status) {
          return udat_toPattern(mDateFormat, /* localized*/ false, target,
                                length, status);
        });
  }

  /**
   * Set the start time of the Gregorian calendar. This is useful for
   * ensuring the consistent use of a proleptic Gregorian calendar for ECMA-402.
   * https://en.wikipedia.org/wiki/Proleptic_Gregorian_calendar
   */
  void SetStartTimeIfGregorian(double aTime);

  ~DateTimeFormat();

  /**
   * TODO(Bug 1686965) - Temporarily get the underlying ICU object while
   * migrating to the unified API. This should be removed when completing the
   * migration.
   */
  UDateFormat* UnsafeGetUDateFormat() const { return mDateFormat; }

  /**
   * Clones the Calendar from a DateTimeFormat, and sets its time with the
   * relative milliseconds since 1 January 1970, UTC.
   */
  Result<UniquePtr<Calendar>, InternalError> CloneCalendar(
      double aUnixEpoch) const;

 private:
  explicit DateTimeFormat(UDateFormat* aDateFormat);

  // mozilla::Vector can avoid heap allocations for small transient buffers.
  static constexpr size_t StackU16VectorSize = 128;

  UDateFormat* mDateFormat = nullptr;
};

}  // namespace mozilla::intl

#endif
