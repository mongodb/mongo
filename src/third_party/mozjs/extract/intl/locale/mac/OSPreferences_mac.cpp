/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "OSPreferences.h"
#include "mozilla/intl/LocaleService.h"
#include <Carbon/Carbon.h>

using namespace mozilla::intl;

static void LocaleChangedNotificationCallback(CFNotificationCenterRef center,
                                              void* observer, CFStringRef name,
                                              const void* object,
                                              CFDictionaryRef userInfo) {
  if (!::CFEqual(name, kCFLocaleCurrentLocaleDidChangeNotification)) {
    return;
  }
  static_cast<OSPreferences*>(observer)->Refresh();
}

OSPreferences::OSPreferences() {
  ::CFNotificationCenterAddObserver(
      ::CFNotificationCenterGetLocalCenter(), this,
      LocaleChangedNotificationCallback,
      kCFLocaleCurrentLocaleDidChangeNotification, 0,
      CFNotificationSuspensionBehaviorDeliverImmediately);
}

bool OSPreferences::ReadSystemLocales(nsTArray<nsCString>& aLocaleList) {
  MOZ_ASSERT(aLocaleList.IsEmpty());

  CFArrayRef langs = ::CFLocaleCopyPreferredLanguages();
  for (CFIndex i = 0; i < ::CFArrayGetCount(langs); i++) {
    CFStringRef lang = (CFStringRef)::CFArrayGetValueAtIndex(langs, i);

    AutoTArray<UniChar, 32> buffer;
    int size = ::CFStringGetLength(lang);
    buffer.SetLength(size);

    CFRange range = ::CFRangeMake(0, size);
    ::CFStringGetCharacters(lang, range, buffer.Elements());

    // Convert the locale string to the format that Mozilla expects
    NS_LossyConvertUTF16toASCII locale(
        reinterpret_cast<const char16_t*>(buffer.Elements()), buffer.Length());

    if (CanonicalizeLanguageTag(locale)) {
      aLocaleList.AppendElement(locale);
    }
  }

  ::CFRelease(langs);

  return !aLocaleList.IsEmpty();
}

bool OSPreferences::ReadRegionalPrefsLocales(nsTArray<nsCString>& aLocaleList) {
  // For now we're just taking System Locales since we don't know of any better
  // API for regional prefs.
  return ReadSystemLocales(aLocaleList);
}

static CFDateFormatterStyle ToCFDateFormatterStyle(
    OSPreferences::DateTimeFormatStyle aFormatStyle) {
  switch (aFormatStyle) {
    case OSPreferences::DateTimeFormatStyle::None:
      return kCFDateFormatterNoStyle;
    case OSPreferences::DateTimeFormatStyle::Short:
      return kCFDateFormatterShortStyle;
    case OSPreferences::DateTimeFormatStyle::Medium:
      return kCFDateFormatterMediumStyle;
    case OSPreferences::DateTimeFormatStyle::Long:
      return kCFDateFormatterLongStyle;
    case OSPreferences::DateTimeFormatStyle::Full:
      return kCFDateFormatterFullStyle;
    case OSPreferences::DateTimeFormatStyle::Invalid:
      MOZ_ASSERT_UNREACHABLE("invalid time format");
      return kCFDateFormatterNoStyle;
  }
}

// Given an 8-bit Gecko string, create a corresponding CFLocale;
// if aLocale is empty, returns a copy of the system's current locale.
// May return null on failure.
// Follows Core Foundation's Create rule, so the caller is responsible to
// release the returned reference.
static CFLocaleRef CreateCFLocaleFor(const nsACString& aLocale) {
  nsAutoCString reqLocale;
  nsAutoCString systemLocale;

  OSPreferences::GetInstance()->GetSystemLocale(systemLocale);

  if (aLocale.IsEmpty()) {
    LocaleService::GetInstance()->GetAppLocaleAsBCP47(reqLocale);
  } else {
    reqLocale.Assign(aLocale);
  }

  bool match = LocaleService::LanguagesMatch(reqLocale, systemLocale);
  if (match) {
    return ::CFLocaleCopyCurrent();
  }

  CFStringRef identifier = CFStringCreateWithBytesNoCopy(
      kCFAllocatorDefault, (const uint8_t*)reqLocale.BeginReading(),
      reqLocale.Length(), kCFStringEncodingASCII, false, kCFAllocatorNull);
  if (!identifier) {
    return nullptr;
  }
  CFLocaleRef locale = CFLocaleCreate(kCFAllocatorDefault, identifier);
  CFRelease(identifier);
  return locale;
}

/**
 * Cocoa API maps nicely to our four styles of date/time.
 *
 * The only caveat is that Cocoa takes regional preferences modifications
 * into account only when we pass an empty string as a locale.
 *
 * In all other cases it will return the default pattern for a given locale.
 */
bool OSPreferences::ReadDateTimePattern(DateTimeFormatStyle aDateStyle,
                                        DateTimeFormatStyle aTimeStyle,
                                        const nsACString& aLocale,
                                        nsACString& aRetVal) {
  CFLocaleRef locale = CreateCFLocaleFor(aLocale);
  if (!locale) {
    return false;
  }

  CFDateFormatterRef formatter = CFDateFormatterCreate(
      kCFAllocatorDefault, locale, ToCFDateFormatterStyle(aDateStyle),
      ToCFDateFormatterStyle(aTimeStyle));
  if (!formatter) {
    return false;
  }
  CFStringRef format = CFDateFormatterGetFormat(formatter);
  CFRelease(locale);

  CFRange range = CFRangeMake(0, CFStringGetLength(format));
  nsAutoString str;
  str.SetLength(range.length);
  CFStringGetCharacters(format, range,
                        reinterpret_cast<UniChar*>(str.BeginWriting()));
  CFRelease(formatter);

  aRetVal = NS_ConvertUTF16toUTF8(str);
  return true;
}

void OSPreferences::RemoveObservers() {
  ::CFNotificationCenterRemoveObserver(
      ::CFNotificationCenterGetLocalCenter(), this,
      kCTFontManagerRegisteredFontsChangedNotification, 0);
}
