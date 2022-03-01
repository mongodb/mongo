/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "mozilla/intl/NumberFormat.h"
#include "NumberFormatFields.h"
#include "NumberFormatterSkeleton.h"
#include "ScopedICUObject.h"

#include "unicode/unumberformatter.h"
#include "unicode/upluralrules.h"

namespace mozilla {
namespace intl {

/*static*/ Result<UniquePtr<NumberFormat>, NumberFormat::FormatError>
NumberFormat::TryCreate(std::string_view aLocale,
                        const NumberFormatOptions& aOptions) {
  UniquePtr<NumberFormat> nf = MakeUnique<NumberFormat>();
  Result<Ok, FormatError> result = nf->initialize(aLocale, aOptions);
  if (result.isOk()) {
    return nf;
  }

  return Err(result.unwrapErr());
}

NumberFormat::~NumberFormat() {
  if (mFormattedNumber) {
    unumf_closeResult(mFormattedNumber);
  }
  if (mNumberFormatter) {
    unumf_close(mNumberFormatter);
  }
}

Result<Ok, NumberFormat::FormatError> NumberFormat::initialize(
    std::string_view aLocale, const NumberFormatOptions& aOptions) {
  mFormatForUnit = aOptions.mUnit.isSome();
  NumberFormatterSkeleton skeleton(aOptions);
  mNumberFormatter = skeleton.toFormatter(aLocale);
  if (mNumberFormatter) {
    UErrorCode status = U_ZERO_ERROR;
    mFormattedNumber = unumf_openResult(&status);
    if (U_SUCCESS(status)) {
      return Ok();
    }
  }
  return Err(FormatError::InternalError);
}

Result<int32_t, NumberFormat::FormatError> NumberFormat::selectFormatted(
    double number, char16_t* keyword, int32_t keywordSize,
    UPluralRules* pluralRules) const {
  MOZ_ASSERT(keyword && pluralRules);
  UErrorCode status = U_ZERO_ERROR;

  if (format(number).isErr()) {
    return Err(NumberFormat::FormatError::InternalError);
  }

  int32_t utf16KeywordLength = uplrules_selectFormatted(
      pluralRules, mFormattedNumber, keyword, keywordSize, &status);

  if (U_FAILURE(status)) {
    return Err(NumberFormat::FormatError::InternalError);
  }

  return utf16KeywordLength;
}

bool NumberFormat::formatInternal(double number) const {
  // ICU incorrectly formats NaN values with the sign bit set, as if they
  // were negative.  Replace all NaNs with a single pattern with sign bit
  // unset ("positive", that is) until ICU is fixed.
  if (MOZ_UNLIKELY(IsNaN(number))) {
    number = SpecificNaN<double>(0, 1);
  }

  UErrorCode status = U_ZERO_ERROR;
  unumf_formatDouble(mNumberFormatter, number, mFormattedNumber, &status);
  return U_SUCCESS(status);
}

bool NumberFormat::formatInternal(int64_t number) const {
  UErrorCode status = U_ZERO_ERROR;
  unumf_formatInt(mNumberFormatter, number, mFormattedNumber, &status);
  return U_SUCCESS(status);
}

bool NumberFormat::formatInternal(std::string_view number) const {
  UErrorCode status = U_ZERO_ERROR;
  unumf_formatDecimal(mNumberFormatter, number.data(), number.size(),
                      mFormattedNumber, &status);
  return U_SUCCESS(status);
}

Result<std::u16string_view, NumberFormat::FormatError>
NumberFormat::formatResult() const {
  UErrorCode status = U_ZERO_ERROR;

  const UFormattedValue* formattedValue =
      unumf_resultAsValue(mFormattedNumber, &status);
  if (U_FAILURE(status)) {
    return Err(FormatError::InternalError);
  }

  int32_t utf16Length;
  const char16_t* utf16Str =
      ufmtval_getString(formattedValue, &utf16Length, &status);
  if (U_FAILURE(status)) {
    return Err(FormatError::InternalError);
  }

  return std::u16string_view(utf16Str, static_cast<size_t>(utf16Length));
}

Maybe<NumberPartType> NumberFormat::GetPartTypeForNumberField(
    UNumberFormatFields fieldName, Maybe<double> number,
    bool isNegative) const {
  switch (fieldName) {
    case UNUM_INTEGER_FIELD:
      if (number.isSome()) {
        if (IsNaN(*number)) {
          return Some(NumberPartType::Nan);
        }
        if (!IsFinite(*number)) {
          return Some(NumberPartType::Infinity);
        }
      }
      return Some(NumberPartType::Integer);
    case UNUM_FRACTION_FIELD:
      return Some(NumberPartType::Fraction);
    case UNUM_DECIMAL_SEPARATOR_FIELD:
      return Some(NumberPartType::Decimal);
    case UNUM_EXPONENT_SYMBOL_FIELD:
      return Some(NumberPartType::ExponentSeparator);
    case UNUM_EXPONENT_SIGN_FIELD:
      return Some(NumberPartType::ExponentMinusSign);
    case UNUM_EXPONENT_FIELD:
      return Some(NumberPartType::ExponentInteger);
    case UNUM_GROUPING_SEPARATOR_FIELD:
      return Some(NumberPartType::Group);
    case UNUM_CURRENCY_FIELD:
      return Some(NumberPartType::Currency);
    case UNUM_PERCENT_FIELD:
      if (mFormatForUnit) {
        return Some(NumberPartType::Unit);
      }
      return Some(NumberPartType::Percent);
    case UNUM_PERMILL_FIELD:
      MOZ_ASSERT_UNREACHABLE(
          "unexpected permill field found, even though "
          "we don't use any user-defined patterns that "
          "would require a permill field");
      break;
    case UNUM_SIGN_FIELD:
      if (isNegative) {
        return Some(NumberPartType::MinusSign);
      }
      return Some(NumberPartType::PlusSign);
    case UNUM_MEASURE_UNIT_FIELD:
      return Some(NumberPartType::Unit);
    case UNUM_COMPACT_FIELD:
      return Some(NumberPartType::Compact);
#ifndef U_HIDE_DEPRECATED_API
    case UNUM_FIELD_COUNT:
      MOZ_ASSERT_UNREACHABLE(
          "format field sentinel value returned by iterator!");
      break;
#endif
  }

  MOZ_ASSERT_UNREACHABLE(
      "unenumerated, undocumented format field returned by iterator");
  return Nothing();
}

Result<std::u16string_view, NumberFormat::FormatError>
NumberFormat::formatResultToParts(Maybe<double> number, bool isNegative,
                                  NumberPartVector& parts) const {
  UErrorCode status = U_ZERO_ERROR;

  const UFormattedValue* formattedValue =
      unumf_resultAsValue(mFormattedNumber, &status);
  if (U_FAILURE(status)) {
    return Err(FormatError::InternalError);
  }

  int32_t utf16Length;
  const char16_t* utf16Str =
      ufmtval_getString(formattedValue, &utf16Length, &status);
  if (U_FAILURE(status)) {
    return Err(FormatError::InternalError);
  }

  UConstrainedFieldPosition* fpos = ucfpos_open(&status);
  if (U_FAILURE(status)) {
    return Err(FormatError::InternalError);
  }
  ScopedICUObject<UConstrainedFieldPosition, ucfpos_close> toCloseFpos(fpos);

  // We're only interested in UFIELD_CATEGORY_NUMBER fields.
  ucfpos_constrainCategory(fpos, UFIELD_CATEGORY_NUMBER, &status);
  if (U_FAILURE(status)) {
    return Err(FormatError::InternalError);
  }

  // Vacuum up fields in the overall formatted string.
  NumberFormatFields fields;

  while (true) {
    bool hasMore = ufmtval_nextPosition(formattedValue, fpos, &status);
    if (U_FAILURE(status)) {
      return Err(FormatError::InternalError);
    }
    if (!hasMore) {
      break;
    }

    int32_t fieldName = ucfpos_getField(fpos, &status);
    if (U_FAILURE(status)) {
      return Err(FormatError::InternalError);
    }

    int32_t beginIndex, endIndex;
    ucfpos_getIndexes(fpos, &beginIndex, &endIndex, &status);
    if (U_FAILURE(status)) {
      return Err(FormatError::InternalError);
    }

    Maybe<NumberPartType> partType = GetPartTypeForNumberField(
        UNumberFormatFields(fieldName), number, isNegative);
    if (!partType || !fields.append(*partType, beginIndex, endIndex)) {
      return Err(FormatError::InternalError);
    }
  }

  if (!fields.toPartsVector(utf16Length, parts)) {
    return Err(FormatError::InternalError);
  }

  return std::u16string_view(utf16Str, static_cast<size_t>(utf16Length));
}

}  // namespace intl
}  // namespace mozilla
