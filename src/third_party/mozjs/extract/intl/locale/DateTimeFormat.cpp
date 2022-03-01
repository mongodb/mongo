/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DateTimeFormat.h"
#include "nsCOMPtr.h"
#include "mozilla/intl/LocaleService.h"
#include "OSPreferences.h"
#include "mozIOSPreferences.h"
#include "unicode/dtfmtsym.h"
#include "unicode/udatpg.h"

namespace mozilla {
using namespace mozilla::intl;

nsCString* DateTimeFormat::mLocale = nullptr;
nsTHashMap<nsCStringHashKey, UDateFormat*>* DateTimeFormat::mFormatCache;

static const int32_t DATETIME_FORMAT_INITIAL_LEN = 127;

/*static*/
nsresult DateTimeFormat::Initialize() {
  if (mLocale) {
    return NS_OK;
  }

  mLocale = new nsCString();
  AutoTArray<nsCString, 10> regionalPrefsLocales;
  intl::LocaleService::GetInstance()->GetRegionalPrefsLocales(
      regionalPrefsLocales);
  mLocale->Assign(regionalPrefsLocales[0]);

  return NS_OK;
}

// performs a locale sensitive date formatting operation on the PRTime parameter
/*static*/
nsresult DateTimeFormat::FormatPRTime(
    const nsDateFormatSelector aDateFormatSelector,
    const nsTimeFormatSelector aTimeFormatSelector, const PRTime aPrTime,
    nsAString& aStringOut) {
  return FormatUDateTime(aDateFormatSelector, aTimeFormatSelector,
                         (aPrTime / PR_USEC_PER_MSEC), nullptr, aStringOut);
}

// performs a locale sensitive date formatting operation on the PRExplodedTime
// parameter
/*static*/
nsresult DateTimeFormat::FormatPRExplodedTime(
    const nsDateFormatSelector aDateFormatSelector,
    const nsTimeFormatSelector aTimeFormatSelector,
    const PRExplodedTime* aExplodedTime, nsAString& aStringOut) {
  return FormatUDateTime(aDateFormatSelector, aTimeFormatSelector,
                         (PR_ImplodeTime(aExplodedTime) / PR_USEC_PER_MSEC),
                         &(aExplodedTime->tm_params), aStringOut);
}

// performs a locale sensitive date formatting operation on the PRExplodedTime
// parameter, using the specified options.
/*static*/
nsresult DateTimeFormat::FormatDateTime(
    const PRExplodedTime* aExplodedTime,
    const DateTimeFormat::Skeleton aSkeleton, nsAString& aStringOut) {
  // set up locale data
  nsresult rv = Initialize();
  if (NS_FAILED(rv)) {
    return rv;
  }

  aStringOut.Truncate();

  UErrorCode status = U_ZERO_ERROR;

  nsAutoCString skeleton;
  switch (aSkeleton) {
    case Skeleton::yyyyMM:
      skeleton.AssignASCII("yyyyMM");
      break;
    case Skeleton::yyyyMMMM:
      skeleton.AssignASCII("yyyyMMMM");
      break;
    case Skeleton::E:
      skeleton.AssignASCII("E");
      break;
    case Skeleton::EEEE:
      skeleton.AssignASCII("EEEE");
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("Unhandled skeleton enum");
  }

  nsAutoCString str;
  if (!OSPreferences::GetPatternForSkeleton(skeleton, *mLocale, str)) {
    return NS_ERROR_FAILURE;
  }
  nsAutoString pattern = NS_ConvertUTF8toUTF16(str);

  nsAutoString timeZoneID;
  BuildTimeZoneString(aExplodedTime->tm_params, timeZoneID);

  UDateFormat* dateTimeFormat =
      udat_open(UDAT_PATTERN, UDAT_PATTERN, mLocale->get(),
                reinterpret_cast<const UChar*>(timeZoneID.BeginReading()),
                timeZoneID.Length(),
                reinterpret_cast<const UChar*>(pattern.BeginReading()),
                pattern.Length(), &status);

  if (U_SUCCESS(status) && dateTimeFormat) {
    UDate udate =
        static_cast<float>((PR_ImplodeTime(aExplodedTime) / PR_USEC_PER_MSEC));

    aStringOut.SetLength(DATETIME_FORMAT_INITIAL_LEN);
    int32_t dateTimeLen =
        udat_format(dateTimeFormat, udate,
                    reinterpret_cast<UChar*>(aStringOut.BeginWriting()),
                    DATETIME_FORMAT_INITIAL_LEN, nullptr, &status);
    aStringOut.SetLength(dateTimeLen);

    if (status == U_BUFFER_OVERFLOW_ERROR) {
      status = U_ZERO_ERROR;
      udat_format(dateTimeFormat, udate,
                  reinterpret_cast<UChar*>(aStringOut.BeginWriting()),
                  dateTimeLen, nullptr, &status);
    }
  }

  udat_close(dateTimeFormat);

  if (U_FAILURE(status)) {
    return NS_ERROR_FAILURE;
  }

  return rv;
}

/*static*/
nsresult DateTimeFormat::GetCalendarSymbol(const Field aField,
                                           const Style aStyle,
                                           const PRExplodedTime* aExplodedTime,
                                           nsAString& aStringOut) {
  nsresult rv = Initialize();
  if (NS_FAILED(rv)) {
    return rv;
  }

  icu::DateFormatSymbols::DtWidthType widthType;
  switch (aStyle) {
    case Style::Wide:
      widthType = icu::DateFormatSymbols::DtWidthType::WIDE;
      break;
    case Style::Abbreviated:
      widthType = icu::DateFormatSymbols::DtWidthType::ABBREVIATED;
      break;
  }

  int32_t count;
  UErrorCode status = U_ZERO_ERROR;
  icu::Locale locale = icu::Locale::createCanonical(mLocale->get());

  UDate udate =
      static_cast<float>((PR_ImplodeTime(aExplodedTime) / PR_USEC_PER_MSEC));

  nsAutoString timeZoneID;
  BuildTimeZoneString(aExplodedTime->tm_params, timeZoneID);
  std::unique_ptr<icu::TimeZone> timeZone(
      icu::TimeZone::createTimeZone(timeZoneID.BeginReading()));
  std::unique_ptr<icu::Calendar> cal(
      icu::Calendar::createInstance(timeZone.release(), locale, status));
  if (U_FAILURE(status)) {
    return NS_ERROR_UNEXPECTED;
  }

  cal->setTime(udate, status);
  if (U_FAILURE(status)) {
    return NS_ERROR_UNEXPECTED;
  }

  std::unique_ptr<icu::DateFormatSymbols> dfs(
      icu::DateFormatSymbols::createForLocale(locale, status));
  if (U_FAILURE(status)) {
    return NS_ERROR_UNEXPECTED;
  }

  if (aField == Field::Month) {
    int32_t month = cal->get(UCAL_MONTH, status);
    if (U_FAILURE(status)) {
      return NS_ERROR_UNEXPECTED;
    }
    const auto* months = dfs->getMonths(
        count, icu::DateFormatSymbols::DtContextType::STANDALONE, widthType);
    if (month < 0 || month >= count) {
      return NS_ERROR_INVALID_ARG;
    }
    aStringOut.Assign(months[month].getBuffer(), months[month].length());
  } else if (aField == Field::Weekday) {
    int32_t weekday = cal->get(UCAL_DAY_OF_WEEK, status);
    if (U_FAILURE(status)) {
      return NS_ERROR_UNEXPECTED;
    }
    const auto* weekdays = dfs->getWeekdays(
        count, icu::DateFormatSymbols::DtContextType::STANDALONE, widthType);
    if (weekday < 0 || weekday >= count) {
      return NS_ERROR_INVALID_ARG;
    }
    aStringOut.Assign(weekdays[weekday].getBuffer(),
                      weekdays[weekday].length());
  }

  return NS_OK;
}

// performs a locale sensitive date formatting operation on the UDate parameter
/*static*/
nsresult DateTimeFormat::FormatUDateTime(
    const nsDateFormatSelector aDateFormatSelector,
    const nsTimeFormatSelector aTimeFormatSelector, const UDate aUDateTime,
    const PRTimeParameters* aTimeParameters, nsAString& aStringOut) {
  int32_t dateTimeLen = 0;
  nsresult rv = NS_OK;

  // return, nothing to format
  if (aDateFormatSelector == kDateFormatNone &&
      aTimeFormatSelector == kTimeFormatNone) {
    aStringOut.Truncate();
    return NS_OK;
  }

  // set up locale data
  rv = Initialize();

  if (NS_FAILED(rv)) {
    return rv;
  }

  UErrorCode status = U_ZERO_ERROR;

  nsAutoCString key;
  key.AppendInt((int)aDateFormatSelector);
  key.Append(':');
  key.AppendInt((int)aTimeFormatSelector);
  if (aTimeParameters) {
    key.Append(':');
    key.AppendInt(aTimeParameters->tp_gmt_offset);
    key.Append(':');
    key.AppendInt(aTimeParameters->tp_dst_offset);
  }

  if (mFormatCache && mFormatCache->Count() == kMaxCachedFormats) {
    // Don't allow a pathological page to extend the cache unreasonably.
    NS_WARNING("flushing UDateFormat cache");
    DeleteCache();
  }
  if (!mFormatCache) {
    mFormatCache =
        new nsTHashMap<nsCStringHashKey, UDateFormat*>(kMaxCachedFormats);
  }

  UDateFormat*& dateTimeFormat = mFormatCache->LookupOrInsert(key);

  if (!dateTimeFormat) {
    // We didn't have a cached formatter for this key, so create one.

    int32_t dateFormatStyle;
    switch (aDateFormatSelector) {
      case kDateFormatLong:
        dateFormatStyle = mozIOSPreferences::dateTimeFormatStyleLong;
        break;
      case kDateFormatShort:
        dateFormatStyle = mozIOSPreferences::dateTimeFormatStyleShort;
        break;
      case kDateFormatNone:
        dateFormatStyle = mozIOSPreferences::dateTimeFormatStyleNone;
        break;
      default:
        NS_ERROR("Unknown nsDateFormatSelector");
        return NS_ERROR_ILLEGAL_VALUE;
    }

    int32_t timeFormatStyle;
    switch (aTimeFormatSelector) {
      case kTimeFormatLong:
        timeFormatStyle = mozIOSPreferences::dateTimeFormatStyleLong;
        break;
      case kTimeFormatShort:
        timeFormatStyle = mozIOSPreferences::dateTimeFormatStyleShort;
        break;
      case kTimeFormatNone:
        timeFormatStyle = mozIOSPreferences::dateTimeFormatStyleNone;
        break;
      default:
        NS_ERROR("Unknown nsDateFormatSelector");
        return NS_ERROR_ILLEGAL_VALUE;
    }

    nsAutoCString str;
    rv = OSPreferences::GetInstance()->GetDateTimePattern(
        dateFormatStyle, timeFormatStyle, nsDependentCString(mLocale->get()),
        str);
    NS_ENSURE_SUCCESS(rv, rv);
    nsAutoString pattern = NS_ConvertUTF8toUTF16(str);

    if (aTimeParameters) {
      nsAutoString timeZoneID;
      BuildTimeZoneString(*aTimeParameters, timeZoneID);

      dateTimeFormat =
          udat_open(UDAT_PATTERN, UDAT_PATTERN, mLocale->get(),
                    reinterpret_cast<const UChar*>(timeZoneID.BeginReading()),
                    timeZoneID.Length(),
                    reinterpret_cast<const UChar*>(pattern.BeginReading()),
                    pattern.Length(), &status);
    } else {
      dateTimeFormat =
          udat_open(UDAT_PATTERN, UDAT_PATTERN, mLocale->get(), nullptr, -1,
                    reinterpret_cast<const UChar*>(pattern.BeginReading()),
                    pattern.Length(), &status);
    }
  }

  if (U_SUCCESS(status) && dateTimeFormat) {
    aStringOut.SetLength(DATETIME_FORMAT_INITIAL_LEN);
    dateTimeLen =
        udat_format(dateTimeFormat, aUDateTime,
                    reinterpret_cast<UChar*>(aStringOut.BeginWriting()),
                    DATETIME_FORMAT_INITIAL_LEN, nullptr, &status);
    aStringOut.SetLength(dateTimeLen);

    if (status == U_BUFFER_OVERFLOW_ERROR) {
      status = U_ZERO_ERROR;
      udat_format(dateTimeFormat, aUDateTime,
                  reinterpret_cast<UChar*>(aStringOut.BeginWriting()),
                  dateTimeLen, nullptr, &status);
    }
  }

  if (U_FAILURE(status)) {
    rv = NS_ERROR_FAILURE;
  }

  return rv;
}

/*static*/
void DateTimeFormat::BuildTimeZoneString(
    const PRTimeParameters& aTimeParameters, nsAString& aStringOut) {
  aStringOut.Truncate();
  aStringOut.Append(u"GMT");
  int32_t totalOffsetMinutes =
      (aTimeParameters.tp_gmt_offset + aTimeParameters.tp_dst_offset) / 60;
  if (totalOffsetMinutes != 0) {
    char sign = totalOffsetMinutes < 0 ? '-' : '+';
    int32_t hours = abs(totalOffsetMinutes) / 60;
    int32_t minutes = abs(totalOffsetMinutes) % 60;
    aStringOut.AppendPrintf("%c%02d:%02d", sign, hours, minutes);
  }
}

/*static*/
void DateTimeFormat::DeleteCache() {
  if (mFormatCache) {
    for (const auto& entry : mFormatCache->Values()) {
      udat_close(entry);
    }
    delete mFormatCache;
    mFormatCache = nullptr;
  }
}

/*static*/
void DateTimeFormat::Shutdown() {
  DeleteCache();
  if (mLocale) {
    delete mLocale;
  }
}

}  // namespace mozilla
