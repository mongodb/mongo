/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/intl/PluralRules.h"

#include "mozilla/intl/NumberFormat.h"
#include "mozilla/Utf8.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Span.h"
#include "ScopedICUObject.h"

#include "unicode/unum.h"
#include "unicode/upluralrules.h"
#include "unicode/ustring.h"

namespace mozilla {
namespace intl {

PluralRules::PluralRules(UPluralRules*& aPluralRules,
                         UniquePtr<NumberFormat>&& aNumberFormat)
    : mPluralRules(aPluralRules), mNumberFormat(std::move(aNumberFormat)) {
  MOZ_ASSERT(aPluralRules);
  aPluralRules = nullptr;
}

Result<UniquePtr<PluralRules>, PluralRules::Error> PluralRules::TryCreate(
    const std::string_view aLocale, const PluralRulesOptions& aOptions) {
  auto numberFormat =
      NumberFormat::TryCreate(aLocale, aOptions.ToNumberFormatOptions());

  if (numberFormat.isErr()) {
    return Err(PluralRules::Error::InternalError);
  }

  UErrorCode status = U_ZERO_ERROR;
  auto pluralType = aOptions.mPluralType == PluralRules::Type::Cardinal
                        ? UPLURAL_TYPE_CARDINAL
                        : UPLURAL_TYPE_ORDINAL;
  UPluralRules* pluralRules =
      uplrules_openForType(aLocale.data(), pluralType, &status);

  if (U_FAILURE(status)) {
    return Err(PluralRules::Error::InternalError);
  }

  return UniquePtr<PluralRules>(
      new PluralRules(pluralRules, numberFormat.unwrap()));
}

Result<PluralRules::Keyword, PluralRules::Error> PluralRules::Select(
    const double aNumber) const {
  char16_t keyword[MAX_KEYWORD_LENGTH];

  auto lengthResult = mNumberFormat->selectFormatted(
      aNumber, keyword, MAX_KEYWORD_LENGTH, mPluralRules);

  if (lengthResult.isErr()) {
    return Err(PluralRules::Error::FormatError);
  }

  return KeywordFromUtf16(Span(keyword, lengthResult.unwrap()));
}

Result<EnumSet<PluralRules::Keyword>, PluralRules::Error>
PluralRules::Categories() const {
  UErrorCode status = U_ZERO_ERROR;
  UEnumeration* enumeration = uplrules_getKeywords(mPluralRules, &status);
  if (U_FAILURE(status)) {
    return Err(PluralRules::Error::InternalError);
  }

  ScopedICUObject<UEnumeration, uenum_close> closeEnum(enumeration);
  EnumSet<PluralRules::Keyword> set;

  while (true) {
    int32_t keywordLength;
    const char* keyword = uenum_next(enumeration, &keywordLength, &status);
    if (U_FAILURE(status)) {
      return Err(PluralRules::Error::InternalError);
    }

    if (!keyword) {
      break;
    }

    set += KeywordFromAscii(Span(keyword, keywordLength));
  }

  return set;
}

PluralRules::Keyword PluralRules::KeywordFromUtf16(
    Span<const char16_t> aKeyword) {
  static constexpr auto kZero = MakeStringSpan(u"zero");
  static constexpr auto kOne = MakeStringSpan(u"one");
  static constexpr auto kTwo = MakeStringSpan(u"two");
  static constexpr auto kFew = MakeStringSpan(u"few");
  static constexpr auto kMany = MakeStringSpan(u"many");

  if (aKeyword == kZero) {
    return PluralRules::Keyword::Zero;
  }
  if (aKeyword == kOne) {
    return PluralRules::Keyword::One;
  }
  if (aKeyword == kTwo) {
    return PluralRules::Keyword::Two;
  }
  if (aKeyword == kFew) {
    return PluralRules::Keyword::Few;
  }
  if (aKeyword == kMany) {
    return PluralRules::Keyword::Many;
  }

  MOZ_ASSERT(aKeyword == MakeStringSpan(u"other"));
  return PluralRules::Keyword::Other;
}

PluralRules::Keyword PluralRules::KeywordFromAscii(Span<const char> aKeyword) {
  static constexpr auto kZero = MakeStringSpan("zero");
  static constexpr auto kOne = MakeStringSpan("one");
  static constexpr auto kTwo = MakeStringSpan("two");
  static constexpr auto kFew = MakeStringSpan("few");
  static constexpr auto kMany = MakeStringSpan("many");

  if (aKeyword == kZero) {
    return PluralRules::Keyword::Zero;
  }
  if (aKeyword == kOne) {
    return PluralRules::Keyword::One;
  }
  if (aKeyword == kTwo) {
    return PluralRules::Keyword::Two;
  }
  if (aKeyword == kFew) {
    return PluralRules::Keyword::Few;
  }
  if (aKeyword == kMany) {
    return PluralRules::Keyword::Many;
  }

  MOZ_ASSERT(aKeyword == MakeStringSpan("other"));
  return PluralRules::Keyword::Other;
}

PluralRules::~PluralRules() {
  if (mPluralRules) {
    uplrules_close(mPluralRules);
    mPluralRules = nullptr;
  }
}

}  // namespace intl
}  // namespace mozilla
