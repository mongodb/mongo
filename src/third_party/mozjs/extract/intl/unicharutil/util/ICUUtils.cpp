/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef MOZILLA_INTERNAL_API

#  include "mozilla/Assertions.h"
#  include "mozilla/UniquePtr.h"

#  include "ICUUtils.h"
#  include "mozilla/StaticPrefs_dom.h"
#  include "mozilla/intl/LocaleService.h"
#  include "nsIContent.h"
#  include "mozilla/dom/Document.h"
#  include "nsString.h"
#  include "unicode/uloc.h"
#  include "unicode/unum.h"

using namespace mozilla;
using mozilla::intl::LocaleService;

class NumberFormatDeleter {
 public:
  void operator()(UNumberFormat* aPtr) {
    MOZ_ASSERT(aPtr != nullptr,
               "UniquePtr deleter shouldn't be called for nullptr");
    unum_close(aPtr);
  }
};

using UniqueUNumberFormat = UniquePtr<UNumberFormat, NumberFormatDeleter>;

void ICUUtils::LanguageTagIterForContent::GetNext(nsACString& aBCP47LangTag) {
  if (mCurrentFallbackIndex < 0) {
    mCurrentFallbackIndex = 0;
    // Try the language specified by a 'lang'/'xml:lang' attribute on mContent
    // or any ancestor, if such an attribute is specified:
    nsAutoString lang;
    mContent->GetLang(lang);
    if (!lang.IsEmpty()) {
      CopyUTF16toUTF8(lang, aBCP47LangTag);
      return;
    }
  }

  if (mCurrentFallbackIndex < 1) {
    mCurrentFallbackIndex = 1;
    // Else try the language specified by any Content-Language HTTP header or
    // pragma directive:
    nsAutoString lang;
    mContent->OwnerDoc()->GetContentLanguage(lang);
    if (!lang.IsEmpty()) {
      CopyUTF16toUTF8(lang, aBCP47LangTag);
      return;
    }
  }

  if (mCurrentFallbackIndex < 2) {
    mCurrentFallbackIndex = 2;
    // Else take the app's locale:

    nsAutoCString appLocale;
    LocaleService::GetInstance()->GetAppLocaleAsBCP47(aBCP47LangTag);
    return;
  }

  // TODO: Probably not worth it, but maybe have a fourth fallback to using
  // the OS locale?

  aBCP47LangTag.Truncate();  // Signal iterator exhausted
}

/* static */
bool ICUUtils::LocalizeNumber(double aValue,
                              LanguageTagIterForContent& aLangTags,
                              nsAString& aLocalizedValue) {
  MOZ_ASSERT(aLangTags.IsAtStart(), "Don't call Next() before passing");

  static const int32_t kBufferSize = 256;

  UChar buffer[kBufferSize];

  nsAutoCString langTag;
  aLangTags.GetNext(langTag);
  while (!langTag.IsEmpty()) {
    UErrorCode status = U_ZERO_ERROR;
    UniqueUNumberFormat format(
        unum_open(UNUM_DECIMAL, nullptr, 0, langTag.get(), nullptr, &status));
    // Since unum_setAttribute have no UErrorCode parameter, we have to
    // check error status.
    if (U_FAILURE(status)) {
      aLangTags.GetNext(langTag);
      continue;
    }
    unum_setAttribute(format.get(), UNUM_GROUPING_USED,
                      StaticPrefs::dom_forms_number_grouping());
    // ICU default is a maximum of 3 significant fractional digits. We don't
    // want that limit, so we set it to the maximum that a double can represent
    // (14-16 decimal fractional digits).
    unum_setAttribute(format.get(), UNUM_MAX_FRACTION_DIGITS, 16);
    int32_t length = unum_formatDouble(format.get(), aValue, buffer,
                                       kBufferSize, nullptr, &status);
    NS_ASSERTION(length < kBufferSize && status != U_BUFFER_OVERFLOW_ERROR &&
                     status != U_STRING_NOT_TERMINATED_WARNING,
                 "Need a bigger buffer?!");
    if (U_SUCCESS(status)) {
      ICUUtils::AssignUCharArrayToString(buffer, length, aLocalizedValue);
      return true;
    }
    aLangTags.GetNext(langTag);
  }
  return false;
}

/* static */
double ICUUtils::ParseNumber(nsAString& aValue,
                             LanguageTagIterForContent& aLangTags) {
  MOZ_ASSERT(aLangTags.IsAtStart(), "Don't call Next() before passing");

  if (aValue.IsEmpty()) {
    return std::numeric_limits<float>::quiet_NaN();
  }

  uint32_t length = aValue.Length();

  nsAutoCString langTag;
  aLangTags.GetNext(langTag);
  while (!langTag.IsEmpty()) {
    UErrorCode status = U_ZERO_ERROR;
    UniqueUNumberFormat format(
        unum_open(UNUM_DECIMAL, nullptr, 0, langTag.get(), nullptr, &status));
    if (!StaticPrefs::dom_forms_number_grouping()) {
      unum_setAttribute(format.get(), UNUM_GROUPING_USED, UBool(0));
    }
    int32_t parsePos = 0;
    static_assert(sizeof(UChar) == 2 && sizeof(nsAString::char_type) == 2,
                  "Unexpected character size - the following cast is unsafe");
    double val = unum_parseDouble(format.get(),
                                  (const UChar*)PromiseFlatString(aValue).get(),
                                  length, &parsePos, &status);
    if (U_SUCCESS(status) && parsePos == (int32_t)length) {
      return val;
    }
    aLangTags.GetNext(langTag);
  }
  return std::numeric_limits<float>::quiet_NaN();
}

/* static */
void ICUUtils::AssignUCharArrayToString(UChar* aICUString, int32_t aLength,
                                        nsAString& aMozString) {
  // Both ICU's UnicodeString and Mozilla's nsAString use UTF-16, so we can
  // cast here.

  static_assert(sizeof(UChar) == 2 && sizeof(nsAString::char_type) == 2,
                "Unexpected character size - the following cast is unsafe");

  aMozString.Assign((const nsAString::char_type*)aICUString, aLength);

  NS_ASSERTION((int32_t)aMozString.Length() == aLength, "Conversion failed");
}

/* static */
nsresult ICUUtils::UErrorToNsResult(const UErrorCode aErrorCode) {
  if (U_SUCCESS(aErrorCode)) {
    return NS_OK;
  }

  switch (aErrorCode) {
    case U_ILLEGAL_ARGUMENT_ERROR:
      return NS_ERROR_INVALID_ARG;

    case U_MEMORY_ALLOCATION_ERROR:
      return NS_ERROR_OUT_OF_MEMORY;

    default:
      return NS_ERROR_FAILURE;
  }
}

#  if 0
/* static */
Locale
ICUUtils::BCP47CodeToLocale(const nsAString& aBCP47Code)
{
  MOZ_ASSERT(!aBCP47Code.IsEmpty(), "Don't pass an empty BCP 47 code");

  Locale locale;
  locale.setToBogus();

  // BCP47 codes are guaranteed to be ASCII, so lossy conversion is okay
  NS_LossyConvertUTF16toASCII bcp47code(aBCP47Code);

  UErrorCode status = U_ZERO_ERROR;
  int32_t needed;

  char localeID[256];
  needed = uloc_forLanguageTag(bcp47code.get(), localeID,
                               PR_ARRAY_SIZE(localeID) - 1, nullptr,
                               &status);
  MOZ_ASSERT(needed < int32_t(PR_ARRAY_SIZE(localeID)) - 1,
             "Need a bigger buffer");
  if (needed <= 0 || U_FAILURE(status)) {
    return locale;
  }

  char lang[64];
  needed = uloc_getLanguage(localeID, lang, PR_ARRAY_SIZE(lang) - 1,
                            &status);
  MOZ_ASSERT(needed < int32_t(PR_ARRAY_SIZE(lang)) - 1,
             "Need a bigger buffer");
  if (needed <= 0 || U_FAILURE(status)) {
    return locale;
  }

  char country[64];
  needed = uloc_getCountry(localeID, country, PR_ARRAY_SIZE(country) - 1,
                           &status);
  MOZ_ASSERT(needed < int32_t(PR_ARRAY_SIZE(country)) - 1,
             "Need a bigger buffer");
  if (needed > 0 && U_SUCCESS(status)) {
    locale = Locale(lang, country);
  }

  if (locale.isBogus()) {
    // Using the country resulted in a bogus Locale, so try with only the lang
    locale = Locale(lang);
  }

  return locale;
}

/* static */
void
ICUUtils::ToMozString(UnicodeString& aICUString, nsAString& aMozString)
{
  // Both ICU's UnicodeString and Mozilla's nsAString use UTF-16, so we can
  // cast here.

  static_assert(sizeof(UChar) == 2 && sizeof(nsAString::char_type) == 2,
                "Unexpected character size - the following cast is unsafe");

  const nsAString::char_type* buf =
    (const nsAString::char_type*)aICUString.getTerminatedBuffer();
  aMozString.Assign(buf);

  NS_ASSERTION(aMozString.Length() == (uint32_t)aICUString.length(),
               "Conversion failed");
}

/* static */
void
ICUUtils::ToICUString(nsAString& aMozString, UnicodeString& aICUString)
{
  // Both ICU's UnicodeString and Mozilla's nsAString use UTF-16, so we can
  // cast here.

  static_assert(sizeof(UChar) == 2 && sizeof(nsAString::char_type) == 2,
                "Unexpected character size - the following cast is unsafe");

  aICUString.setTo((UChar*)PromiseFlatString(aMozString).get(),
                   aMozString.Length());

  NS_ASSERTION(aMozString.Length() == (uint32_t)aICUString.length(),
               "Conversion failed");
}
#  endif

#endif /* MOZILLA_INTERNAL_API */
