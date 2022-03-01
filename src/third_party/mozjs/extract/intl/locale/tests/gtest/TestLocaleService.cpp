/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/intl/LocaleService.h"
#include "mozilla/intl/MozLocale.h"

using namespace mozilla::intl;

TEST(Intl_Locale_LocaleService, CanonicalizeLanguageId)
{
  nsCString locale("en-US.POSIX");
  ASSERT_TRUE(LocaleService::CanonicalizeLanguageId(locale));
  ASSERT_TRUE(locale.EqualsLiteral("en-US"));

  locale.AssignLiteral("en-US_POSIX");
  ASSERT_TRUE(LocaleService::CanonicalizeLanguageId(locale));
  ASSERT_TRUE(locale.EqualsLiteral("en-US-posix"));

  locale.AssignLiteral("en-US-POSIX");
  ASSERT_TRUE(LocaleService::CanonicalizeLanguageId(locale));
  ASSERT_TRUE(locale.EqualsLiteral("en-US-posix"));

  locale.AssignLiteral("C");
  ASSERT_FALSE(LocaleService::CanonicalizeLanguageId(locale));
  ASSERT_TRUE(locale.EqualsLiteral("und"));

  locale.AssignLiteral("");
  ASSERT_FALSE(LocaleService::CanonicalizeLanguageId(locale));
  ASSERT_TRUE(locale.EqualsLiteral("und"));
}

TEST(Intl_Locale_LocaleService, GetAppLocalesAsBCP47)
{
  nsTArray<nsCString> appLocales;
  LocaleService::GetInstance()->GetAppLocalesAsBCP47(appLocales);

  ASSERT_FALSE(appLocales.IsEmpty());
}

TEST(Intl_Locale_LocaleService, GetAppLocalesAsLangTags)
{
  nsTArray<nsCString> appLocales;
  LocaleService::GetInstance()->GetAppLocalesAsLangTags(appLocales);

  ASSERT_FALSE(appLocales.IsEmpty());
}

TEST(Intl_Locale_LocaleService, GetAppLocalesAsLangTags_lastIsPresent)
{
  nsAutoCString lastFallbackLocale;
  LocaleService::GetInstance()->GetLastFallbackLocale(lastFallbackLocale);

  nsTArray<nsCString> appLocales;
  LocaleService::GetInstance()->GetAppLocalesAsLangTags(appLocales);

  ASSERT_TRUE(appLocales.Contains(lastFallbackLocale));
}

TEST(Intl_Locale_LocaleService, GetAppLocaleAsLangTag)
{
  nsTArray<nsCString> appLocales;
  LocaleService::GetInstance()->GetAppLocalesAsLangTags(appLocales);

  nsAutoCString locale;
  LocaleService::GetInstance()->GetAppLocaleAsLangTag(locale);

  ASSERT_TRUE(appLocales[0] == locale);
}

TEST(Intl_Locale_LocaleService, GetRegionalPrefsLocales)
{
  nsTArray<nsCString> rpLocales;
  LocaleService::GetInstance()->GetRegionalPrefsLocales(rpLocales);

  int32_t len = rpLocales.Length();
  ASSERT_TRUE(len > 0);
}

TEST(Intl_Locale_LocaleService, GetWebExposedLocales)
{
  const nsTArray<nsCString> spoofLocale{"de"_ns};
  LocaleService::GetInstance()->SetAvailableLocales(spoofLocale);
  LocaleService::GetInstance()->SetRequestedLocales(spoofLocale);

  nsTArray<nsCString> pvLocales;

  mozilla::Preferences::SetInt("privacy.spoof_english", 0);
  LocaleService::GetInstance()->GetWebExposedLocales(pvLocales);
  ASSERT_TRUE(pvLocales.Length() > 0);
  ASSERT_TRUE(pvLocales[0].Equals("de"_ns));

  mozilla::Preferences::SetCString("intl.locale.privacy.web_exposed", "zh-TW");
  LocaleService::GetInstance()->GetWebExposedLocales(pvLocales);
  ASSERT_TRUE(pvLocales.Length() > 0);
  ASSERT_TRUE(pvLocales[0].Equals("zh-TW"_ns));

  mozilla::Preferences::SetInt("privacy.spoof_english", 2);
  LocaleService::GetInstance()->GetWebExposedLocales(pvLocales);
  ASSERT_EQ(1u, pvLocales.Length());
  ASSERT_TRUE(pvLocales[0].Equals("en-US"_ns));
}

TEST(Intl_Locale_LocaleService, GetRequestedLocales)
{
  nsTArray<nsCString> reqLocales;
  LocaleService::GetInstance()->GetRequestedLocales(reqLocales);

  int32_t len = reqLocales.Length();
  ASSERT_TRUE(len > 0);
}

TEST(Intl_Locale_LocaleService, GetAvailableLocales)
{
  nsTArray<nsCString> availableLocales;
  LocaleService::GetInstance()->GetAvailableLocales(availableLocales);

  int32_t len = availableLocales.Length();
  ASSERT_TRUE(len > 0);
}

TEST(Intl_Locale_LocaleService, GetPackagedLocales)
{
  nsTArray<nsCString> packagedLocales;
  LocaleService::GetInstance()->GetPackagedLocales(packagedLocales);

  int32_t len = packagedLocales.Length();
  ASSERT_TRUE(len > 0);
}

TEST(Intl_Locale_LocaleService, GetDefaultLocale)
{
  nsAutoCString locStr;
  LocaleService::GetInstance()->GetDefaultLocale(locStr);

  ASSERT_FALSE(locStr.IsEmpty());
  ASSERT_TRUE(Locale(locStr).IsWellFormed());
}

TEST(Intl_Locale_LocaleService, IsAppLocaleRTL)
{
  mozilla::Preferences::SetCString("intl.l10n.pseudo", "bidi");
  ASSERT_TRUE(LocaleService::GetInstance()->IsAppLocaleRTL());
  mozilla::Preferences::ClearUser("intl.l10n.pseudo");
}
