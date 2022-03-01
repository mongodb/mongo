/* -*- Mode: C++; tab-width: 2; indent-tabs-mode:nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Quotes.h"
#include "MozLocale.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/StaticPtr.h"
#include "nsTHashMap.h"
#include "nsPrintfCString.h"

using namespace mozilla;
using namespace mozilla::intl;

namespace {
struct LangQuotesRec {
  const char* mLangs;
  Quotes mQuotes;
};

#include "cldr-quotes.inc"

static StaticAutoPtr<nsTHashMap<nsCStringHashKey, Quotes>> sQuotesForLang;
}  // anonymous namespace

namespace mozilla {
namespace intl {

const Quotes* QuotesForLang(const nsAtom* aLang) {
  MOZ_ASSERT(NS_IsMainThread());

  // On first use, initialize the hashtable from our CLDR-derived data array.
  if (!sQuotesForLang) {
    sQuotesForLang = new nsTHashMap<nsCStringHashKey, Quotes>(32);
    ClearOnShutdown(&sQuotesForLang);
    for (const auto& i : sLangQuotes) {
      const char* s = i.mLangs;
      size_t len;
      while ((len = strlen(s))) {
        sQuotesForLang->InsertOrUpdate(nsDependentCString(s, len), i.mQuotes);
        s += len + 1;
      }
    }
  }

  nsAtomCString langStr(aLang);
  const Quotes* entry = sQuotesForLang->Lookup(langStr).DataPtrOrNull();
  if (entry) {
    // Found an exact match for the requested lang.
    return entry;
  }

  // Try parsing lang as a Locale (which will also canonicalize case of the
  // subtags), then see if we can match it with region or script subtags,
  // if present, or just the primary language tag.
  Locale loc(langStr);
  if (!loc.IsWellFormed()) {
    return nullptr;
  }
  if (!loc.GetRegion().IsEmpty()) {
    nsAutoCString langAndRegion;
    langAndRegion.Append(loc.GetLanguage());
    langAndRegion.Append('-');
    langAndRegion.Append(loc.GetRegion());
    if ((entry = sQuotesForLang->Lookup(langAndRegion).DataPtrOrNull())) {
      return entry;
    }
  }
  if (!loc.GetScript().IsEmpty()) {
    nsAutoCString langAndScript;
    langAndScript.Append(loc.GetLanguage());
    langAndScript.Append('-');
    langAndScript.Append(loc.GetScript());
    if ((entry = sQuotesForLang->Lookup(langAndScript).DataPtrOrNull())) {
      return entry;
    }
  }
  if ((entry = sQuotesForLang->Lookup(loc.GetLanguage()).DataPtrOrNull())) {
    return entry;
  }

  return nullptr;
}

}  // namespace intl
}  // namespace mozilla
