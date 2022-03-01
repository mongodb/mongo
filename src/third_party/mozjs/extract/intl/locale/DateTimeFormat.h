/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_DateTimeFormat_h
#define mozilla_DateTimeFormat_h

#include <time.h>
#include "gtest/MozGtestFriend.h"
#include "nsTHashMap.h"
#include "nsString.h"
#include "prtime.h"
#include "unicode/udat.h"

namespace mozilla {

enum nsDateFormatSelector : long {
  // Do not change the order of the values below (see bug 1225696).
  kDateFormatNone = 0,  // do not include the date  in the format string
  kDateFormatLong,      // provides the long date format for the given locale
  kDateFormatShort,     // provides the short date format for the given locale
};

enum nsTimeFormatSelector : long {
  kTimeFormatNone = 0,  // don't include the time in the format string
  kTimeFormatLong,      // provides the time format with seconds in the given
                        // locale
  kTimeFormatShort      // provides the time format without seconds in the given
                        // locale
};

class DateTimeFormat {
 public:
  enum class Field { Month, Weekday };

  enum class Style { Wide, Abbreviated };

  // Weekday (E, EEEE) only used in Thunderbird.
  enum class Skeleton { yyyyMM, yyyyMMMM, E, EEEE };

  // performs a locale sensitive date formatting operation on the PRTime
  // parameter
  static nsresult FormatPRTime(const nsDateFormatSelector aDateFormatSelector,
                               const nsTimeFormatSelector aTimeFormatSelector,
                               const PRTime aPrTime, nsAString& aStringOut);

  // performs a locale sensitive date formatting operation on the PRExplodedTime
  // parameter
  static nsresult FormatPRExplodedTime(
      const nsDateFormatSelector aDateFormatSelector,
      const nsTimeFormatSelector aTimeFormatSelector,
      const PRExplodedTime* aExplodedTime, nsAString& aStringOut);

  // performs a locale sensitive date formatting operation on the PRExplodedTime
  // parameter, using the specified options.
  static nsresult FormatDateTime(const PRExplodedTime* aExplodedTime,
                                 const Skeleton aSkeleton,
                                 nsAString& aStringOut);

  // finds the locale sensitive display name for the specified field on the
  // PRExplodedTime parameter
  static nsresult GetCalendarSymbol(const Field aField, const Style aStyle,
                                    const PRExplodedTime* aExplodedTime,
                                    nsAString& aStringOut);

  static void Shutdown();

 private:
  DateTimeFormat() = delete;

  static nsresult Initialize();
  static void DeleteCache();
  static const size_t kMaxCachedFormats = 15;

  FRIEND_TEST(DateTimeFormat, FormatPRExplodedTime);
  FRIEND_TEST(DateTimeFormat, DateFormatSelectors);
  FRIEND_TEST(DateTimeFormat, FormatPRExplodedTimeForeign);
  FRIEND_TEST(DateTimeFormat, DateFormatSelectorsForeign);

  // performs a locale sensitive date formatting operation on the UDate
  // parameter
  static nsresult FormatUDateTime(
      const nsDateFormatSelector aDateFormatSelector,
      const nsTimeFormatSelector aTimeFormatSelector, const UDate aUDateTime,
      const PRTimeParameters* aTimeParameters, nsAString& aStringOut);

  static void BuildTimeZoneString(const PRTimeParameters& aTimeParameters,
                                  nsAString& aStringOut);

  static nsCString* mLocale;
  static nsTHashMap<nsCStringHashKey, UDateFormat*>* mFormatCache;
};

}  // namespace mozilla

#endif /* mozilla_DateTimeFormat_h */
