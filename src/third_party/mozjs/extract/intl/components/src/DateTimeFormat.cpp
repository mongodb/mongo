/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "unicode/ucal.h"
#include "unicode/udat.h"
#include "unicode/udatpg.h"

#include "ScopedICUObject.h"

#include "mozilla/intl/Calendar.h"
#include "mozilla/intl/DateTimeFormat.h"

namespace mozilla::intl {

DateTimeFormat::~DateTimeFormat() {
  MOZ_ASSERT(mDateFormat);
  udat_close(mDateFormat);
}

static UDateFormatStyle ToUDateFormatStyle(DateTimeStyle aStyle) {
  switch (aStyle) {
    case DateTimeStyle::Full:
      return UDAT_FULL;
    case DateTimeStyle::Long:
      return UDAT_LONG;
    case DateTimeStyle::Medium:
      return UDAT_MEDIUM;
    case DateTimeStyle::Short:
      return UDAT_SHORT;
    case DateTimeStyle::None:
      return UDAT_NONE;
  }
  MOZ_ASSERT_UNREACHABLE();
  // Do not use the default: branch so that the enum is exhaustively checked.
  return UDAT_NONE;
}

/* static */
Result<UniquePtr<DateTimeFormat>, DateTimeFormat::StyleError>
DateTimeFormat::TryCreateFromStyle(
    Span<const char> aLocale, DateTimeStyle aDateStyle,
    DateTimeStyle aTimeStyle, Maybe<Span<const char16_t>> aTimeZoneOverride) {
  auto dateStyle = ToUDateFormatStyle(aDateStyle);
  auto timeStyle = ToUDateFormatStyle(aTimeStyle);

  if (dateStyle == UDAT_NONE && timeStyle == UDAT_NONE) {
    dateStyle = UDAT_DEFAULT;
    timeStyle = UDAT_DEFAULT;
  }

  // The time zone is optional.
  int32_t tzIDLength = -1;
  const UChar* tzID = nullptr;
  if (aTimeZoneOverride) {
    tzIDLength = static_cast<int32_t>(aTimeZoneOverride->size());
    tzID = aTimeZoneOverride->Elements();
  }

  UErrorCode status = U_ZERO_ERROR;
  UDateFormat* dateFormat =
      udat_open(dateStyle, timeStyle, aLocale.data(), tzID, tzIDLength,
                /* pattern */ nullptr, /* pattern length */ -1, &status);

  if (U_SUCCESS(status)) {
    return UniquePtr<DateTimeFormat>(new DateTimeFormat(dateFormat));
  }

  return Err(DateTimeFormat::StyleError::DateFormatFailure);
}

DateTimeFormat::DateTimeFormat(UDateFormat* aDateFormat) {
  MOZ_RELEASE_ASSERT(aDateFormat, "Expected aDateFormat to not be a nullptr.");
  mDateFormat = aDateFormat;
}

/* static */
Result<UniquePtr<DateTimeFormat>, DateTimeFormat::PatternError>
DateTimeFormat::TryCreateFromPattern(
    Span<const char> aLocale, Span<const char16_t> aPattern,
    Maybe<Span<const char16_t>> aTimeZoneOverride) {
  UErrorCode status = U_ZERO_ERROR;

  // The time zone is optional.
  int32_t tzIDLength = -1;
  const UChar* tzID = nullptr;
  if (aTimeZoneOverride) {
    tzIDLength = static_cast<int32_t>(aTimeZoneOverride->size());
    tzID = aTimeZoneOverride->data();
  }

  // Create the date formatter.
  UDateFormat* dateFormat = udat_open(
      UDAT_PATTERN, UDAT_PATTERN, static_cast<const char*>(aLocale.data()),
      tzID, tzIDLength, aPattern.data(), static_cast<int32_t>(aPattern.size()),
      &status);

  if (U_FAILURE(status)) {
    return Err(PatternError::DateFormatFailure);
  }

  // The DateTimeFormat wrapper will control the life cycle of the ICU
  // dateFormat object.
  return UniquePtr<DateTimeFormat>(new DateTimeFormat(dateFormat));
}

/* static */
Result<UniquePtr<DateTimeFormat>, DateTimeFormat::SkeletonError>
DateTimeFormat::TryCreateFromSkeleton(
    Span<const char> aLocale, Span<const char16_t> aSkeleton,
    Maybe<Span<const char16_t>> aTimeZoneOverride) {
  UErrorCode status = U_ZERO_ERROR;

  // Create a time pattern generator. Its lifetime is scoped to this function.
  UDateTimePatternGenerator* dtpg = udatpg_open(aLocale.data(), &status);
  if (U_FAILURE(status)) {
    return Err(SkeletonError::PatternGeneratorFailure);
  }
  ScopedICUObject<UDateTimePatternGenerator, udatpg_close> datPgToClose(dtpg);

  // Compute the best pattern for the skeleton.
  mozilla::Vector<char16_t, DateTimeFormat::StackU16VectorSize> bestPattern;

  auto result = FillVectorWithICUCall(
      bestPattern,
      [&dtpg, &aSkeleton](UChar* target, int32_t length, UErrorCode* status) {
        return udatpg_getBestPattern(dtpg, aSkeleton.data(),
                                     static_cast<int32_t>(aSkeleton.size()),
                                     target, length, status);
      });

  if (result.isErr()) {
    return Err(SkeletonError::GetBestPatternFailure);
  }

  return DateTimeFormat::TryCreateFromPattern(aLocale, bestPattern,
                                              aTimeZoneOverride)
      .mapErr([](DateTimeFormat::PatternError error) {
        switch (error) {
          case DateTimeFormat::PatternError::DateFormatFailure:
            return SkeletonError::DateFormatFailure;
        }
        // Do not use the default branch, so that the switch is exhaustively
        // checked.
        MOZ_ASSERT_UNREACHABLE();
        return SkeletonError::DateFormatFailure;
      });
}

/* static */
Result<UniquePtr<DateTimeFormat>, DateTimeFormat::SkeletonError>
DateTimeFormat::TryCreateFromSkeleton(
    Span<const char> aLocale, Span<const char> aSkeleton,
    Maybe<Span<const char>> aTimeZoneOverride) {
  // Convert the skeleton to UTF-16.
  mozilla::Vector<char16_t, DateTimeFormat::StackU16VectorSize>
      skeletonUtf16Buffer;

  if (!FillUTF16Vector(aSkeleton, skeletonUtf16Buffer)) {
    return Err(SkeletonError::OutOfMemory);
  }

  // Convert the timezone to UTF-16 if it exists.
  mozilla::Vector<char16_t, DateTimeFormat::StackU16VectorSize> tzUtf16Vec;
  Maybe<Span<const char16_t>> timeZone = Nothing{};
  if (aTimeZoneOverride) {
    if (!FillUTF16Vector(*aTimeZoneOverride, tzUtf16Vec)) {
      return Err(SkeletonError::OutOfMemory);
    };
    timeZone =
        Some(Span<const char16_t>(tzUtf16Vec.begin(), tzUtf16Vec.length()));
  }

  return DateTimeFormat::TryCreateFromSkeleton(aLocale, skeletonUtf16Buffer,
                                               timeZone);
}

void DateTimeFormat::SetStartTimeIfGregorian(double aTime) {
  UErrorCode status = U_ZERO_ERROR;
  UCalendar* cal = const_cast<UCalendar*>(udat_getCalendar(mDateFormat));
  ucal_setGregorianChange(cal, aTime, &status);
  // An error here means the calendar is not Gregorian, and can be ignored.
}

/* static */
Result<UniquePtr<Calendar>, InternalError> DateTimeFormat::CloneCalendar(
    double aUnixEpoch) const {
  UErrorCode status = U_ZERO_ERROR;
  UCalendar* calendarRaw = ucal_clone(udat_getCalendar(mDateFormat), &status);
  if (U_FAILURE(status)) {
    return Err(InternalError{});
  }
  auto calendar = MakeUnique<Calendar>(calendarRaw);

  auto setTimeResult = calendar->SetTimeInMs(aUnixEpoch);
  if (setTimeResult.isErr()) {
    return Err(InternalError{});
  }
  return calendar;
}

}  // namespace mozilla::intl
