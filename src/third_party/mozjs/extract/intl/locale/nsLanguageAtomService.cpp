/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsLanguageAtomService.h"
#include "nsUConvPropertySearch.h"
#include "nsUnicharUtils.h"
#include "nsAtom.h"
#include "nsGkAtoms.h"
#include "mozilla/ArrayUtils.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Encoding.h"
#include "mozilla/intl/OSPreferences.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/ServoUtils.h"

using namespace mozilla;
using mozilla::intl::OSPreferences;

static constexpr nsUConvProp encodingsGroups[] = {
#include "encodingsgroups.properties.h"
};

// List of mozilla internal x-* tags that map to themselves (see bug 256257)
static constexpr nsStaticAtom* kLangGroups[] = {
    // This list must be sorted!
    nsGkAtoms::x_armn,  nsGkAtoms::x_cyrillic, nsGkAtoms::x_devanagari,
    nsGkAtoms::x_geor,  nsGkAtoms::x_math,     nsGkAtoms::x_tamil,
    nsGkAtoms::Unicode, nsGkAtoms::x_western
    // These self-mappings are not necessary unless somebody use them to specify
    // lang in (X)HTML/XML documents, which they shouldn't. (see bug 256257)
    // x-beng=x-beng
    // x-cans=x-cans
    // x-ethi=x-ethi
    // x-guru=x-guru
    // x-gujr=x-gujr
    // x-khmr=x-khmr
    // x-mlym=x-mlym
};

// Map ISO 15924 script codes from BCP47 lang tag to mozilla's langGroups.
static constexpr struct {
  const char* mTag;
  nsStaticAtom* mAtom;
} kScriptLangGroup[] = {
    // This list must be sorted by script code!
    {"Arab", nsGkAtoms::ar},
    {"Armn", nsGkAtoms::x_armn},
    {"Beng", nsGkAtoms::x_beng},
    {"Cans", nsGkAtoms::x_cans},
    {"Cyrl", nsGkAtoms::x_cyrillic},
    {"Deva", nsGkAtoms::x_devanagari},
    {"Ethi", nsGkAtoms::x_ethi},
    {"Geok", nsGkAtoms::x_geor},
    {"Geor", nsGkAtoms::x_geor},
    {"Grek", nsGkAtoms::el},
    {"Gujr", nsGkAtoms::x_gujr},
    {"Guru", nsGkAtoms::x_guru},
    {"Hang", nsGkAtoms::ko},
    // Hani is not mapped to a specific langGroup, we prefer to look at the
    // primary language subtag in this case
    {"Hans", nsGkAtoms::Chinese},
    // Hant is special-cased in code
    // Hant=zh-HK
    // Hant=zh-TW
    {"Hebr", nsGkAtoms::he},
    {"Hira", nsGkAtoms::Japanese},
    {"Jpan", nsGkAtoms::Japanese},
    {"Kana", nsGkAtoms::Japanese},
    {"Khmr", nsGkAtoms::x_khmr},
    {"Knda", nsGkAtoms::x_knda},
    {"Kore", nsGkAtoms::ko},
    {"Latn", nsGkAtoms::x_western},
    {"Mlym", nsGkAtoms::x_mlym},
    {"Orya", nsGkAtoms::x_orya},
    {"Sinh", nsGkAtoms::x_sinh},
    {"Taml", nsGkAtoms::x_tamil},
    {"Telu", nsGkAtoms::x_telu},
    {"Thai", nsGkAtoms::th},
    {"Tibt", nsGkAtoms::x_tibt}};

static UniquePtr<nsLanguageAtomService> gLangAtomService;

// static
nsLanguageAtomService* nsLanguageAtomService::GetService() {
  if (!gLangAtomService) {
    gLangAtomService = MakeUnique<nsLanguageAtomService>();
  }
  return gLangAtomService.get();
}

// static
void nsLanguageAtomService::Shutdown() { gLangAtomService = nullptr; }

nsStaticAtom* nsLanguageAtomService::LookupLanguage(
    const nsACString& aLanguage) {
  nsAutoCString lowered(aLanguage);
  ToLowerCase(lowered);

  RefPtr<nsAtom> lang = NS_Atomize(lowered);
  return GetLanguageGroup(lang);
}

already_AddRefed<nsAtom> nsLanguageAtomService::LookupCharSet(
    NotNull<const Encoding*> aEncoding) {
  nsAutoCString charset;
  aEncoding->Name(charset);
  nsAutoCString group;
  if (NS_FAILED(nsUConvPropertySearch::SearchPropertyValue(
          encodingsGroups, ArrayLength(encodingsGroups), charset, group))) {
    return RefPtr<nsAtom>(nsGkAtoms::Unicode).forget();
  }
  return NS_Atomize(group);
}

nsAtom* nsLanguageAtomService::GetLocaleLanguage() {
  do {
    if (!mLocaleLanguage) {
      AutoTArray<nsCString, 10> regionalPrefsLocales;
      if (NS_SUCCEEDED(OSPreferences::GetInstance()->GetRegionalPrefsLocales(
              regionalPrefsLocales))) {
        // use lowercase for all language atoms
        ToLowerCase(regionalPrefsLocales[0]);
        mLocaleLanguage = NS_Atomize(regionalPrefsLocales[0]);
      } else {
        nsAutoCString locale;
        OSPreferences::GetInstance()->GetSystemLocale(locale);

        ToLowerCase(locale);  // use lowercase for all language atoms
        mLocaleLanguage = NS_Atomize(locale);
      }
    }
  } while (0);

  return mLocaleLanguage;
}

nsStaticAtom* nsLanguageAtomService::GetLanguageGroup(nsAtom* aLanguage,
                                                      bool* aNeedsToCache) {
  if (aNeedsToCache) {
    if (nsStaticAtom* atom = mLangToGroup.Get(aLanguage)) {
      return atom;
    }
    *aNeedsToCache = true;
    return nullptr;
  }

  return mLangToGroup.LookupOrInsertWith(aLanguage, [&] {
    AssertIsMainThreadOrServoFontMetricsLocked();
    return GetUncachedLanguageGroup(aLanguage);
  });
}

nsStaticAtom* nsLanguageAtomService::GetUncachedLanguageGroup(
    nsAtom* aLanguage) const {
  nsAutoCString langStr;
  aLanguage->ToUTF8String(langStr);
  ToLowerCase(langStr);

  if (langStr[0] == 'x' && langStr[1] == '-') {
    // Internal x-* langGroup codes map to themselves (see bug 256257)
    for (nsStaticAtom* langGroup : kLangGroups) {
      if (langGroup == aLanguage) {
        return langGroup;
      }
      if (aLanguage->IsAsciiLowercase()) {
        continue;
      }
      // Do the slow ascii-case-insensitive comparison just if needed.
      nsDependentAtomString string(langGroup);
      if (string.EqualsASCII(langStr.get(), langStr.Length())) {
        return langGroup;
      }
    }
  } else {
    // If the lang code can be parsed as BCP47, look up its (likely) script.

    // https://bugzilla.mozilla.org/show_bug.cgi?id=1618034:
    // First strip any private subtags that would cause Locale to reject the
    // tag as non-wellformed.
    nsACString::const_iterator start, end;
    langStr.BeginReading(start);
    langStr.EndReading(end);
    if (FindInReadable("-x-"_ns, start, end)) {
      // The substring we want ends at the beginning of the "-x-" subtag.
      langStr.Truncate(start.get() - langStr.BeginReading());
    }

    Locale loc(langStr);
    if (loc.IsWellFormed()) {
      // Fill in script subtag if not present.
      if (loc.GetScript().IsEmpty()) {
        loc.Maximize();
      }
      // Traditional Chinese has separate prefs for Hong Kong / Taiwan;
      // check the region subtag.
      if (loc.GetScript().EqualsLiteral("Hant")) {
        if (loc.GetRegion().EqualsLiteral("HK")) {
          return nsGkAtoms::HongKongChinese;
        }
        return nsGkAtoms::Taiwanese;
      }
      // Search list of known script subtags that map to langGroup codes.
      size_t foundIndex;
      nsDependentCSubstring script = loc.GetScript();
      if (BinarySearchIf(
              kScriptLangGroup, 0, ArrayLength(kScriptLangGroup),
              [script](const auto& entry) -> int {
                return script.Compare(entry.mTag);
              },
              &foundIndex)) {
        return kScriptLangGroup[foundIndex].mAtom;
      }
      // Script subtag was not recognized (includes "Hani"); check the language
      // subtag for CJK possibilities so that we'll prefer the appropriate font
      // rather than falling back to the browser's hardcoded preference.
      if (loc.GetLanguage().EqualsLiteral("zh")) {
        if (loc.GetRegion().EqualsLiteral("HK")) {
          return nsGkAtoms::HongKongChinese;
        }
        if (loc.GetRegion().EqualsLiteral("TW")) {
          return nsGkAtoms::Taiwanese;
        }
        return nsGkAtoms::Chinese;
      }
      if (loc.GetLanguage().EqualsLiteral("ja")) {
        return nsGkAtoms::Japanese;
      }
      if (loc.GetLanguage().EqualsLiteral("ko")) {
        return nsGkAtoms::ko;
      }
    }
  }

  // Fall back to x-unicode if no match was found
  return nsGkAtoms::Unicode;
}
