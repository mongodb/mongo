/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ICUUtils_h__
#define mozilla_ICUUtils_h__

// The ICU utils implementation needs internal things like XPCOM strings and
// nsGkAtom, so we only build when included into internal libs:
#ifdef MOZILLA_INTERNAL_API

#  include "nsString.h"
#  include "unicode/unum.h"  // for UNumberFormat

class nsIContent;

class ICUUtils {
 public:
  /**
   * This class is used to encapsulate an nsIContent object to allow lazy
   * iteration over its primary and fallback BCP 47 language tags.
   */
  class LanguageTagIterForContent {
   public:
    explicit LanguageTagIterForContent(nsIContent* aContent)
        : mContent(aContent), mCurrentFallbackIndex(-1) {}

    /**
     * Used to iterate over the nsIContent object's primary language tag and
     * its fallbacks tags. The following sources of language tag information
     * are tried in turn:
     *
     * 1) the "lang" of the nsIContent object (which is based on the 'lang'/
     *    'xml:lang' attribute on itself or the nearest ancestor to have such
     *    an attribute, if any);
     * 2) the Content-Language HTTP pragma directive or HTTP header;
     * 3) the configured language tag of the user-agent.
     *
     * Once all fallbacks have been exhausted then this function will set
     * aBCP47LangTag to the empty string.
     */
    void GetNext(nsACString& aBCP47LangTag);

    bool IsAtStart() const { return mCurrentFallbackIndex < 0; }

   private:
    nsIContent* mContent;
    int8_t mCurrentFallbackIndex;
  };

  /**
   * Attempts to localize aValue and return the result via the aLocalizedValue
   * outparam. Returns true on success. Returns false on failure, in which
   * case aLocalizedValue will be untouched.
   */
  static bool LocalizeNumber(double aValue,
                             LanguageTagIterForContent& aLangTags,
                             nsAString& aLocalizedValue);

  /**
   * Parses the localized number that is serialized in aValue using aLangTags
   * and returns the result as a double. Returns NaN on failure.
   */
  static double ParseNumber(nsAString& aValue,
                            LanguageTagIterForContent& aLangTags);

  static void AssignUCharArrayToString(UChar* aICUString, int32_t aLength,
                                       nsAString& aMozString);

  /**
   * Map ICU UErrorCode to nsresult
   */
  static nsresult UErrorToNsResult(const UErrorCode aErrorCode);

#  if 0
  // Currently disabled because using C++ API doesn't play nicely with enabling
  // system ICU.

  /**
   * Converts an IETF BCP 47 language code to an ICU Locale.
   */
  static Locale BCP47CodeToLocale(const nsAString& aBCP47Code);

  static void ToMozString(UnicodeString& aICUString, nsAString& aMozString);
  static void ToICUString(nsAString& aMozString, UnicodeString& aICUString);
#  endif
};

#endif /* MOZILLA_INTERNAL_API */

#endif /* mozilla_ICUUtils_h__ */
