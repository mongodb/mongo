/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef intl_components_Calendar_h_
#define intl_components_Calendar_h_
#include "unicode/ucal.h"

#include "mozilla/Assertions.h"
#include "mozilla/intl/DateTimeFormat.h"
#include "mozilla/intl/ICU4CGlue.h"
#include "mozilla/Maybe.h"
#include "mozilla/Result.h"
#include "mozilla/ResultVariant.h"
#include "mozilla/Span.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Utf8.h"
#include "mozilla/Vector.h"

namespace mozilla::intl {

/**
 * This component is a Mozilla-focused API for working with calendar systems in
 * internationalization code. It is used in coordination with other operations
 * such as datetime formatting.
 */
class Calendar final {
 public:
  explicit Calendar(UCalendar* aCalendar) : mCalendar(aCalendar) {
    MOZ_ASSERT(aCalendar);
  };

  // Do not allow copy as this class owns the ICU resource. Move is not
  // currently implemented, but a custom move operator could be created if
  // needed.
  Calendar(const Calendar&) = delete;
  Calendar& operator=(const Calendar&) = delete;

  enum class Error { InternalError };

  /**
   * Create a Calendar.
   */
  static Result<UniquePtr<Calendar>, Calendar::Error> TryCreate(
      const char* aLocale,
      Maybe<Span<const char16_t>> aTimeZoneOverride = Nothing{});

  /**
   * Get the BCP 47 keyword value string designating the calendar type. For
   * instance "gregory", "chinese", "islamicc", etc.
   */
  Result<const char*, Calendar::Error> GetBcp47Type();

  /**
   * A number indicating the raw offset from GMT in milliseconds.
   */
  Result<int32_t, Calendar::Error> GetDefaultTimeZoneOffsetMs();

  /**
   * Fill the buffer with the system's default IANA time zone identifier, e.g.
   * "America/Chicago".
   */
  template <typename B>
  static ICUResult GetDefaultTimeZone(B& aBuffer) {
    return FillBufferWithICUCall(aBuffer, ucal_getDefaultTimeZone);
  }

  /**
   * Returns the canonical system time zone ID or the normalized custom time
   * zone ID for the given time zone ID.
   */
  template <typename B>
  static ICUResult GetCanonicalTimeZoneID(Span<const char16_t> inputTimeZone,
                                          B& aBuffer) {
    static_assert(std::is_same_v<typename B::CharType, char16_t>,
                  "Currently only UTF-16 buffers are supported.");

    if (aBuffer.capacity() == 0) {
      // ucal_getCanonicalTimeZoneID differs from other API calls and fails when
      // passed a nullptr or 0 length result. Reserve some space initially so
      // that a real pointer will be used in the API.
      //
      // At the time of this writing 32 characters fits every time zone listed
      // in: https://en.wikipedia.org/wiki/List_of_tz_database_time_zones
      // https://gist.github.com/gregtatum/f926de157a44e5965864da866fe71e63
      if (!aBuffer.reserve(32)) {
        return Err(ICUError::OutOfMemory);
      }
    }

    return FillBufferWithICUCall(
        aBuffer,
        [&inputTimeZone](UChar* target, int32_t length, UErrorCode* status) {
          return ucal_getCanonicalTimeZoneID(
              inputTimeZone.Elements(),
              static_cast<int32_t>(inputTimeZone.Length()), target, length,
              /* isSystemID */ nullptr, status);
        });
  }

  /**
   * Set the time for the calendar relative to the number of milliseconds since
   * 1 January 1970, UTC.
   */
  Result<Ok, Error> SetTimeInMs(double aUnixEpoch);

  /**
   * Return ICU legacy keywords, such as "gregorian", "islamic",
   * "islamic-civil", "hebrew", etc.
   */
  static Result<SpanEnumeration<char>, InternalError>
  GetLegacyKeywordValuesForLocale(const char* aLocale);

 private:
  /**
   * Internal function to convert a legacy calendar identifier to the newer
   * BCP 47 identifier.
   */
  static SpanResult<char> LegacyIdentifierToBcp47(const char* aIdentifier,
                                                  int32_t aLength);

 public:
  using Bcp47IdentifierEnumeration =
      Enumeration<char, SpanResult<char>, Calendar::LegacyIdentifierToBcp47>;
  /**
   * Return BCP 47 Unicode locale extension type keywords.
   */
  static Result<Bcp47IdentifierEnumeration, InternalError>
  GetBcp47KeywordValuesForLocale(const char* aLocale);

  ~Calendar();

  /**
   * TODO(Bug 1686965) - Temporarily get the underlying ICU object while
   * migrating to the unified API. This should be removed when completing the
   * migration.
   */
  UCalendar* UnsafeGetUCalendar() const { return mCalendar; }

 private:
  UCalendar* mCalendar = nullptr;
};

}  // namespace mozilla::intl

#endif
