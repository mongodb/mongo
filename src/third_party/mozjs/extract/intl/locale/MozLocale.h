/* -*- Mode: C++; tab-width: 2; indent-tabs-mode:nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_intl_MozLocale_h__
#define mozilla_intl_MozLocale_h__

#include "nsString.h"
#include "nsTArray.h"
#include "MozLocaleBindings.h"

namespace mozilla {
namespace intl {

/**
 * Locale class is a core representation of a single locale.
 *
 * A locale is a data object representing a combination of language, script,
 * region, variant and a set of regional extension preferences that may further
 * specify particular user choices like calendar, numbering system, etc.
 *
 * A locale can be expressed as a language tag string, like a simple "fr" for
 * French, or a more specific "sr-Cyrl-RS-u-hc-h12" for Serbian in Russia with a
 * Cyrylic script, and hour cycle selected to be `h12`.
 *
 * The format of the language tag follows BCP47 standard and implements a subset
 * of it. In the future we expect to extend this class to cover more subtags and
 * extensions.
 *
 * BCP47: https://tools.ietf.org/html/bcp47
 *
 * The aim of this class it aid in validation, parsing and canonicalization of
 * the string.
 *
 * It allows the user to input any well-formed BCP47 language tag and operate
 * on its subtags in a canonicalized form.
 *
 * It should be used for all operations on language tags, and together with
 * LocaleService::NegotiateLanguages for language negotiation.
 *
 * Example:
 *
 *     Locale loc = Locale("de-at");
 *
 *     ASSERT_TRUE(loc.GetLanguage().Equals("de"));
 *     ASSERT_TRUE(loc.GetScript().IsEmpty());
 *     ASSERT_TRUE(loc.GetRegion().Equals("AT")); // canonicalized to upper case
 *
 *
 * Note: The file name is `MozLocale` to avoid compilation problems on
 * case-insensitive Windows. The class name is `Locale`.
 */
class Locale {
 public:
  /**
   * The constructor expects the input to be a well-formed BCP47-style locale
   * string.
   *
   * Two operations are performed on the well-formed language tags:
   *
   *  * Case normalization to conform with BCP47 (e.g. "eN-uS" -> "en-US")
   *  * Underscore delimiters replaced with dashed (e.g. "en_US" -> "en-US")
   *
   * If the input language tag string is not well-formed, the Locale will be
   * created with its flag `mWellFormed` set to false which will make the Locale
   * never match.
   */
  explicit Locale(const nsACString& aLocale);
  explicit Locale(const char* aLocale) : Locale(nsDependentCString(aLocale)){};

  const nsDependentCSubstring GetLanguage() const;
  const nsDependentCSubstring GetScript() const;
  const nsDependentCSubstring GetRegion() const;
  void GetVariants(nsTArray<nsCString>& aRetVal) const;

  /**
   * Returns a `true` if the locale is well-formed, such that the
   * Locale object can validly be matched against others.
   */
  bool IsWellFormed() const { return mIsWellFormed; }

  /**
   * Returns a canonicalized language tag string of the locale.
   */
  const nsCString AsString() const;

  /**
   * Compares two locales optionally treating one or both of
   * the locales as a range.
   *
   * In case one of the locales is treated as a range, its
   * empty fields are considered to match all possible
   * values of the same field on the other locale.
   *
   * Example:
   *
   * Locale("en").Matches(Locale("en-US"), false, false) // false
   * Locale("en").Matches(Locale("en-US"), true, false)  // true
   *
   * The latter returns true because the region field on the "en"
   * locale is being treated as a range and matches any region field
   * value including "US" of the other locale.
   */
  bool Matches(const Locale& aOther, bool aThisRange, bool aOtherRange) const;

  /**
   * This operation uses CLDR data to build a more specific version
   * of a generic locale.
   *
   * Example:
   *
   * Locale("en").Maximize().AsString(); // "en-Latn-US"
   */
  bool Maximize();

  /**
   * Clears the variants field of the Locale object.
   */
  void ClearVariants();

  /**
   * Clears the region field of the Locale object.
   */
  void ClearRegion();

  /**
   * Marks the locale as invalid which in turns makes
   * it to be skipped by most LocaleService operations.
   */
  void Invalidate() { mIsWellFormed = false; }

  /**
   * Compares two locales expecting all fields to match each other.
   */
  bool operator==(const Locale& aOther) {
    // Note: non-well-formed Locale objects are never
    // treated as equal to anything
    // (even other non-well-formed ones).
    return Matches(aOther, false, false);
  }

  Locale(Locale&& aOther)
      : mIsWellFormed(aOther.mIsWellFormed), mRaw(std::move(aOther.mRaw)) {}

  ffi::LanguageIdentifier* Raw() { return mRaw.get(); }
  const ffi::LanguageIdentifier* Raw() const { return mRaw.get(); }

 private:
  bool mIsWellFormed;

  // Notice. We had to remove `const` to allow for move constructor, but
  // that makes it possible to nullptr `mRaw`. Just don't.
  UniquePtr<ffi::LanguageIdentifier> mRaw;
};

}  // namespace intl
}  // namespace mozilla

MOZ_DECLARE_RELOCATE_USING_MOVE_CONSTRUCTOR(mozilla::intl::Locale)

#endif /* mozilla_intl_MozLocale_h__ */
