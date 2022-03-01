/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/Preferences.h"
#include "mozilla/intl/OSPreferences.h"

using namespace mozilla::intl;

/**
 * We test that on all platforms we test against (irrelevant of the tier),
 * we will be able to retrieve at least a single locale out of the system.
 *
 * In theory, that may not be true, but if we encounter such platform we should
 * decide how to handle this and special case and this test should make
 * it not happen without us noticing.
 */
TEST(Intl_Locale_OSPreferences, GetSystemLocales)
{
  nsTArray<nsCString> systemLocales;
  ASSERT_TRUE(NS_SUCCEEDED(
      OSPreferences::GetInstance()->GetSystemLocales(systemLocales)));

  ASSERT_FALSE(systemLocales.IsEmpty());
}

/**
 * We test that on all platforms we test against (irrelevant of the tier),
 * we will be able to retrieve at least a single locale out of the system.
 *
 * In theory, that may not be true, but if we encounter such platform we should
 * decide how to handle this and special case and this test should make
 * it not happen without us noticing.
 */
TEST(Intl_Locale_OSPreferences, GetRegionalPrefsLocales)
{
  nsTArray<nsCString> rgLocales;
  ASSERT_TRUE(NS_SUCCEEDED(
      OSPreferences::GetInstance()->GetRegionalPrefsLocales(rgLocales)));

  ASSERT_FALSE(rgLocales.IsEmpty());
}

/**
 * We test that on all platforms we test against,
 * we will be able to retrieve a date and time pattern.
 *
 * This may come back empty on platforms where we don't have platforms
 * bindings for, so effectively, we're testing for crashes. We should
 * never crash.
 */
TEST(Intl_Locale_OSPreferences, GetDateTimePattern)
{
  nsAutoCString pattern;
  OSPreferences* osprefs = OSPreferences::GetInstance();

  struct Test {
    int dateStyle;
    int timeStyle;
    const char* locale;
  };
  Test tests[] = {{0, 0, ""},   {1, 0, "pl"}, {2, 0, "de-DE"}, {3, 0, "fr"},
                  {4, 0, "ar"},

                  {0, 1, ""},   {0, 2, "it"}, {0, 3, ""},      {0, 4, "ru"},

                  {4, 1, ""},   {3, 2, "cs"}, {2, 3, ""},      {1, 4, "ja"}};

  for (unsigned i = 0; i < mozilla::ArrayLength(tests); i++) {
    const Test& t = tests[i];
    if (NS_SUCCEEDED(osprefs->GetDateTimePattern(
            t.dateStyle, t.timeStyle, nsDependentCString(t.locale), pattern))) {
      ASSERT_TRUE((t.dateStyle == 0 && t.timeStyle == 0) || !pattern.IsEmpty());
    }
  }

  // If the locale is not specified, we should get the pattern corresponding to
  // the first regional prefs locale.
  AutoTArray<nsCString, 10> rpLocales;
  LocaleService::GetInstance()->GetRegionalPrefsLocales(rpLocales);
  ASSERT_TRUE(rpLocales.Length() > 0);

  nsAutoCString rpLocalePattern;
  ASSERT_TRUE(NS_SUCCEEDED(
      osprefs->GetDateTimePattern(mozIOSPreferences::dateTimeFormatStyleLong,
                                  mozIOSPreferences::dateTimeFormatStyleLong,
                                  rpLocales[0], rpLocalePattern)));
  ASSERT_TRUE(NS_SUCCEEDED(
      osprefs->GetDateTimePattern(mozIOSPreferences::dateTimeFormatStyleLong,
                                  mozIOSPreferences::dateTimeFormatStyleLong,
                                  nsDependentCString(""), pattern)));
  ASSERT_EQ(rpLocalePattern, pattern);
}

/**
 * Test that is possible to override the OS defaults through a pref.
 */
TEST(Intl_Locale_OSPreferences, GetDateTimePatternPrefOverrides)
{
  nsresult nr;
  nsAutoCString default_pattern, pattern;
  OSPreferences* osprefs = OSPreferences::GetInstance();

  struct {
    const char* DatePref;
    const char* TimePref;
    int32_t DateTimeFormatStyle;
  } configs[] = {{"intl.date_time.pattern_override.date_short",
                  "intl.date_time.pattern_override.time_short",
                  mozIOSPreferences::dateTimeFormatStyleShort},
                 {"intl.date_time.pattern_override.date_medium",
                  "intl.date_time.pattern_override.time_medium",
                  mozIOSPreferences::dateTimeFormatStyleMedium},
                 {"intl.date_time.pattern_override.date_long",
                  "intl.date_time.pattern_override.time_long",
                  mozIOSPreferences::dateTimeFormatStyleLong},
                 {"intl.date_time.pattern_override.date_full",
                  "intl.date_time.pattern_override.time_full",
                  mozIOSPreferences::dateTimeFormatStyleFull}};

  for (const auto& config : configs) {
    // Get default value for the OS
    nr = osprefs->GetDateTimePattern(config.DateTimeFormatStyle,
                                     mozIOSPreferences::dateTimeFormatStyleNone,
                                     nsDependentCString(""), default_pattern);
    ASSERT_TRUE(NS_SUCCEEDED(nr));

    // Override date format
    mozilla::Preferences::SetCString(config.DatePref, "yy-MM");
    nr = osprefs->GetDateTimePattern(config.DateTimeFormatStyle,
                                     mozIOSPreferences::dateTimeFormatStyleNone,
                                     nsDependentCString(""), pattern);
    ASSERT_TRUE(NS_SUCCEEDED(nr));
    ASSERT_TRUE(pattern.EqualsASCII("yy-MM"));

    // Override time format
    mozilla::Preferences::SetCString(config.TimePref, "HH:mm");
    nr = osprefs->GetDateTimePattern(mozIOSPreferences::dateTimeFormatStyleNone,
                                     config.DateTimeFormatStyle,
                                     nsDependentCString(""), pattern);
    ASSERT_TRUE(NS_SUCCEEDED(nr));
    ASSERT_TRUE(pattern.EqualsASCII("HH:mm"));

    // Override both
    nr = osprefs->GetDateTimePattern(config.DateTimeFormatStyle,
                                     config.DateTimeFormatStyle,
                                     nsDependentCString(""), pattern);
    ASSERT_TRUE(NS_SUCCEEDED(nr));
    ASSERT_TRUE(pattern.Find("yy-MM") != kNotFound);
    ASSERT_TRUE(pattern.Find("HH:mm") != kNotFound);

    // Clear overrides, we should get the default value back.
    mozilla::Preferences::ClearUser(config.DatePref);
    mozilla::Preferences::ClearUser(config.TimePref);
    nr = osprefs->GetDateTimePattern(config.DateTimeFormatStyle,
                                     mozIOSPreferences::dateTimeFormatStyleNone,
                                     nsDependentCString(""), pattern);
    ASSERT_TRUE(NS_SUCCEEDED(nr));
    ASSERT_EQ(default_pattern, pattern);
  }

  // Test overriding connector
  nr = osprefs->GetDateTimePattern(mozIOSPreferences::dateTimeFormatStyleShort,
                                   mozIOSPreferences::dateTimeFormatStyleShort,
                                   nsDependentCString(""), default_pattern);

  mozilla::Preferences::SetCString("intl.date_time.pattern_override.date_short",
                                   "yyyy-MM-dd");
  mozilla::Preferences::SetCString("intl.date_time.pattern_override.time_short",
                                   "HH:mm:ss");
  mozilla::Preferences::SetCString(
      "intl.date_time.pattern_override.connector_short", "{1} {0}");
  nr = osprefs->GetDateTimePattern(mozIOSPreferences::dateTimeFormatStyleShort,
                                   mozIOSPreferences::dateTimeFormatStyleShort,
                                   nsDependentCString(""), pattern);
  ASSERT_TRUE(NS_SUCCEEDED(nr));
  ASSERT_TRUE(pattern.EqualsASCII("yyyy-MM-dd HH:mm:ss"));

  // Reset to date and time to defaults
  mozilla::Preferences::ClearUser("intl.date_time.pattern_override.date_short");
  mozilla::Preferences::ClearUser("intl.date_time.pattern_override.time_short");

  // Invalid patterns are ignored
  mozilla::Preferences::SetCString(
      "intl.date_time.pattern_override.connector_short", "hello, world!");
  nr = osprefs->GetDateTimePattern(mozIOSPreferences::dateTimeFormatStyleShort,
                                   mozIOSPreferences::dateTimeFormatStyleShort,
                                   nsDependentCString(""), pattern);
  ASSERT_TRUE(NS_SUCCEEDED(nr));
  ASSERT_EQ(default_pattern, pattern);

  // Clearing the override results in getting the default pattern back.
  mozilla::Preferences::ClearUser(
      "intl.date_time.pattern_override.connector_short");
  nr = osprefs->GetDateTimePattern(mozIOSPreferences::dateTimeFormatStyleShort,
                                   mozIOSPreferences::dateTimeFormatStyleShort,
                                   nsDependentCString(""), pattern);
  ASSERT_TRUE(NS_SUCCEEDED(nr));
  ASSERT_EQ(default_pattern, pattern);
}
