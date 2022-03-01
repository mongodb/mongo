/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "OSPreferences.h"
#include "mozilla/intl/LocaleService.h"
#include "mozilla/intl/MozLocale.h"
#include "mozilla/WindowsVersion.h"
#include "nsReadableUtils.h"

#include <windows.h>

#ifndef __MINGW32__  // WinRT headers not yet supported by MinGW
#  include <roapi.h>
#  include <wrl.h>
#  include <Windows.System.UserProfile.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::Foundation::Collections;
using namespace ABI::Windows::System::UserProfile;
#endif

using namespace mozilla::intl;

OSPreferences::OSPreferences() {}

bool OSPreferences::ReadSystemLocales(nsTArray<nsCString>& aLocaleList) {
  MOZ_ASSERT(aLocaleList.IsEmpty());

#ifndef __MINGW32__
  if (IsWin8OrLater()) {
    // Try to get language list from GlobalizationPreferences; if this fails,
    // we'll fall back to GetUserPreferredUILanguages.
    // Per MSDN, these APIs are not available prior to Win8.

    // RoInitialize may fail with "cannot change thread mode after it is set",
    // if the runtime was already initialized on this thread.
    // This appears to be harmless, and we can proceed to attempt the following
    // runtime calls.
    HRESULT inited = RoInitialize(RO_INIT_MULTITHREADED);
    if (SUCCEEDED(inited) || inited == RPC_E_CHANGED_MODE) {
      ComPtr<IGlobalizationPreferencesStatics> globalizationPrefs;
      ComPtr<IVectorView<HSTRING>> languages;
      uint32_t count;
      if (SUCCEEDED(RoGetActivationFactory(
              HStringReference(
                  RuntimeClass_Windows_System_UserProfile_GlobalizationPreferences)
                  .Get(),
              IID_PPV_ARGS(&globalizationPrefs))) &&
          SUCCEEDED(globalizationPrefs->get_Languages(&languages)) &&
          SUCCEEDED(languages->get_Size(&count))) {
        for (uint32_t i = 0; i < count; ++i) {
          HString lang;
          if (SUCCEEDED(languages->GetAt(i, lang.GetAddressOf()))) {
            unsigned int length;
            const wchar_t* text = lang.GetRawBuffer(&length);
            NS_LossyConvertUTF16toASCII loc(text, length);
            if (CanonicalizeLanguageTag(loc)) {
              if (!loc.Contains('-')) {
                // DirectWrite font-name code doesn't like to be given a bare
                // language code with no region subtag, but the
                // GlobalizationPreferences API may give us one (e.g. "ja").
                // So if there's no hyphen in the string at this point, we use
                // Locale::Maximize to get a suitable region code to
                // go with it.
                Locale locale(loc);
                if (locale.Maximize() && !locale.GetRegion().IsEmpty()) {
                  loc.Append('-');
                  loc.Append(locale.GetRegion());
                }
              }
              aLocaleList.AppendElement(loc);
            }
          }
        }
      }
    }
    // Only close the runtime if we successfully initialized it above,
    // otherwise we assume it was already in use and should be left as is.
    if (SUCCEEDED(inited)) {
      RoUninitialize();
    }
  }
#endif

  // Per MSDN, GetUserPreferredUILanguages is available from Vista onwards,
  // so we can use it unconditionally (although it may not work well!)
  if (aLocaleList.IsEmpty()) {
    // Note that according to the questioner at
    // https://stackoverflow.com/questions/52849233/getuserpreferreduilanguages-never-returns-more-than-two-languages,
    // this may not always return the full list of languages we'd expect.
    // We should always get at least the first-preference lang, though.
    ULONG numLanguages = 0;
    DWORD cchLanguagesBuffer = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &numLanguages, nullptr,
                                     &cchLanguagesBuffer)) {
      return false;
    }

    AutoTArray<WCHAR, 64> locBuffer;
    locBuffer.SetCapacity(cchLanguagesBuffer);
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &numLanguages,
                                     locBuffer.Elements(),
                                     &cchLanguagesBuffer)) {
      return false;
    }

    const WCHAR* start = locBuffer.Elements();
    const WCHAR* bufEnd = start + cchLanguagesBuffer;
    while (bufEnd - start > 1 && *start) {
      const WCHAR* end = start + 1;
      while (bufEnd - end > 1 && *end) {
        end++;
      }
      NS_LossyConvertUTF16toASCII loc(start, end - start);
      if (CanonicalizeLanguageTag(loc)) {
        aLocaleList.AppendElement(loc);
      }
      start = end + 1;
    }
  }

  return !aLocaleList.IsEmpty();
}

bool OSPreferences::ReadRegionalPrefsLocales(nsTArray<nsCString>& aLocaleList) {
  MOZ_ASSERT(aLocaleList.IsEmpty());

  WCHAR locale[LOCALE_NAME_MAX_LENGTH];
  if (NS_WARN_IF(!LCIDToLocaleName(LOCALE_USER_DEFAULT, locale,
                                   LOCALE_NAME_MAX_LENGTH, 0))) {
    return false;
  }

  NS_LossyConvertUTF16toASCII loc(locale);

  if (CanonicalizeLanguageTag(loc)) {
    aLocaleList.AppendElement(loc);
    return true;
  }
  return false;
}

static LCTYPE ToDateLCType(OSPreferences::DateTimeFormatStyle aFormatStyle) {
  switch (aFormatStyle) {
    case OSPreferences::DateTimeFormatStyle::None:
      return LOCALE_SLONGDATE;
    case OSPreferences::DateTimeFormatStyle::Short:
      return LOCALE_SSHORTDATE;
    case OSPreferences::DateTimeFormatStyle::Medium:
      return LOCALE_SSHORTDATE;
    case OSPreferences::DateTimeFormatStyle::Long:
      return LOCALE_SLONGDATE;
    case OSPreferences::DateTimeFormatStyle::Full:
      return LOCALE_SLONGDATE;
    case OSPreferences::DateTimeFormatStyle::Invalid:
    default:
      MOZ_ASSERT_UNREACHABLE("invalid date format");
      return LOCALE_SLONGDATE;
  }
}

static LCTYPE ToTimeLCType(OSPreferences::DateTimeFormatStyle aFormatStyle) {
  switch (aFormatStyle) {
    case OSPreferences::DateTimeFormatStyle::None:
      return LOCALE_STIMEFORMAT;
    case OSPreferences::DateTimeFormatStyle::Short:
      return LOCALE_SSHORTTIME;
    case OSPreferences::DateTimeFormatStyle::Medium:
      return LOCALE_SSHORTTIME;
    case OSPreferences::DateTimeFormatStyle::Long:
      return LOCALE_STIMEFORMAT;
    case OSPreferences::DateTimeFormatStyle::Full:
      return LOCALE_STIMEFORMAT;
    case OSPreferences::DateTimeFormatStyle::Invalid:
    default:
      MOZ_ASSERT_UNREACHABLE("invalid time format");
      return LOCALE_STIMEFORMAT;
  }
}

/**
 * Windows API includes regional preferences from the user only
 * if we pass empty locale string or if the locale string matches
 * the current locale.
 *
 * Since Windows API only allows us to retrieve two options - short/long
 * we map it to our four options as:
 *
 *   short  -> short
 *   medium -> short
 *   long   -> long
 *   full   -> long
 *
 * In order to produce a single date/time format, we use CLDR pattern
 * for combined date/time string, since Windows API does not provide an
 * option for this.
 */
bool OSPreferences::ReadDateTimePattern(DateTimeFormatStyle aDateStyle,
                                        DateTimeFormatStyle aTimeStyle,
                                        const nsACString& aLocale,
                                        nsACString& aRetVal) {
  nsAutoString localeName;
  CopyASCIItoUTF16(aLocale, localeName);

  bool isDate = aDateStyle != DateTimeFormatStyle::None &&
                aDateStyle != DateTimeFormatStyle::Invalid;
  bool isTime = aTimeStyle != DateTimeFormatStyle::None &&
                aTimeStyle != DateTimeFormatStyle::Invalid;

  // If both date and time are wanted, we'll initially read them into a
  // local string, and then insert them into the overall date+time pattern;
  nsAutoString str;
  if (isDate && isTime) {
    if (!GetDateTimeConnectorPattern(aLocale, aRetVal)) {
      NS_WARNING("failed to get date/time connector");
      aRetVal.AssignLiteral("{1} {0}");
    }
  } else if (!isDate && !isTime) {
    aRetVal.Truncate(0);
    return true;
  }

  if (isDate) {
    LCTYPE lcType = ToDateLCType(aDateStyle);
    size_t len = GetLocaleInfoEx(
        reinterpret_cast<const wchar_t*>(localeName.BeginReading()), lcType,
        nullptr, 0);
    if (len == 0) {
      return false;
    }

    // We're doing it to ensure the terminator will fit when Windows writes the
    // data to its output buffer. See bug 1358159 for details.
    str.SetLength(len);
    GetLocaleInfoEx(reinterpret_cast<const wchar_t*>(localeName.BeginReading()),
                    lcType, (WCHAR*)str.BeginWriting(), len);
    str.SetLength(len - 1);  // -1 because len counts the null terminator

    // Windows uses "ddd" and "dddd" for abbreviated and full day names
    // respectively,
    //   https://msdn.microsoft.com/en-us/library/windows/desktop/dd317787(v=vs.85).aspx
    // but in a CLDR/ICU-style pattern these should be "EEE" and "EEEE".
    //   http://userguide.icu-project.org/formatparse/datetime
    // So we fix that up here.
    nsAString::const_iterator start, pos, end;
    start = str.BeginReading(pos);
    str.EndReading(end);
    if (FindInReadable(u"dddd"_ns, pos, end)) {
      str.ReplaceLiteral(pos - start, 4, u"EEEE");
    } else {
      pos = start;
      if (FindInReadable(u"ddd"_ns, pos, end)) {
        str.ReplaceLiteral(pos - start, 3, u"EEE");
      }
    }

    // Also, Windows uses lowercase "g" or "gg" for era, but ICU wants uppercase
    // "G" (it would interpret "g" as "modified Julian day"!). So fix that.
    int32_t index = str.FindChar('g');
    if (index >= 0) {
      str.Replace(index, 1, 'G');
      // If it was a double "gg", just drop the second one.
      index++;
      if (str.CharAt(index) == 'g') {
        str.Cut(index, 1);
      }
    }

    // If time was also requested, we need to substitute the date pattern from
    // Windows into the date+time format that we have in aRetVal.
    if (isTime) {
      nsACString::const_iterator start, pos, end;
      start = aRetVal.BeginReading(pos);
      aRetVal.EndReading(end);
      if (FindInReadable("{1}"_ns, pos, end)) {
        aRetVal.Replace(pos - start, 3, NS_ConvertUTF16toUTF8(str));
      }
    } else {
      aRetVal = NS_ConvertUTF16toUTF8(str);
    }
  }

  if (isTime) {
    LCTYPE lcType = ToTimeLCType(aTimeStyle);
    size_t len = GetLocaleInfoEx(
        reinterpret_cast<const wchar_t*>(localeName.BeginReading()), lcType,
        nullptr, 0);
    if (len == 0) {
      return false;
    }

    // We're doing it to ensure the terminator will fit when Windows writes the
    // data to its output buffer. See bug 1358159 for details.
    str.SetLength(len);
    GetLocaleInfoEx(reinterpret_cast<const wchar_t*>(localeName.BeginReading()),
                    lcType, (WCHAR*)str.BeginWriting(), len);
    str.SetLength(len - 1);

    // Windows uses "t" or "tt" for a "time marker" (am/pm indicator),
    //   https://msdn.microsoft.com/en-us/library/windows/desktop/dd318148(v=vs.85).aspx
    // but in a CLDR/ICU-style pattern that should be "a".
    //   http://userguide.icu-project.org/formatparse/datetime
    // So we fix that up here.
    int32_t index = str.FindChar('t');
    if (index >= 0) {
      str.Replace(index, 1, 'a');
      index++;
      if (str.CharAt(index) == 't') {
        str.Cut(index, 1);
      }
    }

    if (isDate) {
      nsACString::const_iterator start, pos, end;
      start = aRetVal.BeginReading(pos);
      aRetVal.EndReading(end);
      if (FindInReadable("{0}"_ns, pos, end)) {
        aRetVal.Replace(pos - start, 3, NS_ConvertUTF16toUTF8(str));
      }
    } else {
      aRetVal = NS_ConvertUTF16toUTF8(str);
    }
  }

  return true;
}

void OSPreferences::RemoveObservers() {}
