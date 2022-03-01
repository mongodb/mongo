/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "NumberFormatterSkeleton.h"
#include "NumberFormat.h"

#include "MeasureUnitGenerated.h"

#include <algorithm>

namespace mozilla {
namespace intl {

NumberFormatterSkeleton::NumberFormatterSkeleton(
    const NumberFormatOptions& options) {
  if (options.mCurrency.isSome()) {
    if (!currency(options.mCurrency->first) ||
        !currencyDisplay(options.mCurrency->second)) {
      return;
    }
  } else if (options.mUnit.isSome()) {
    if (!unit(options.mUnit->first) || !unitDisplay(options.mUnit->second)) {
      return;
    }
  } else if (options.mPercent) {
    if (!percent()) {
      return;
    }
  }

  if (options.mFractionDigits.isSome()) {
    if (!fractionDigits(options.mFractionDigits->first,
                        options.mFractionDigits->second)) {
      return;
    }
  }

  if (options.mMinIntegerDigits.isSome()) {
    if (!minIntegerDigits(*options.mMinIntegerDigits)) {
      return;
    }
  }

  if (options.mSignificantDigits.isSome()) {
    if (!significantDigits(options.mSignificantDigits->first,
                           options.mSignificantDigits->second)) {
      return;
    }
  }

  if (!options.mUseGrouping) {
    if (!disableGrouping()) {
      return;
    }
  }

  if (!notation(options.mNotation)) {
    return;
  }

  if (!signDisplay(options.mSignDisplay)) {
    return;
  }

  if (options.mRoundingModeHalfUp) {
    if (!roundingModeHalfUp()) {
      return;
    }
  }

  mValidSkeleton = true;
}

bool NumberFormatterSkeleton::currency(std::string_view currency) {
  MOZ_ASSERT(currency.size() == 3,
             "IsWellFormedCurrencyCode permits only length-3 strings");

  char16_t currencyChars[] = {static_cast<char16_t>(currency[0]),
                              static_cast<char16_t>(currency[1]),
                              static_cast<char16_t>(currency[2]), '\0'};
  return append(u"currency/") && append(currencyChars) && append(' ');
}

bool NumberFormatterSkeleton::currencyDisplay(
    NumberFormatOptions::CurrencyDisplay display) {
  switch (display) {
    case NumberFormatOptions::CurrencyDisplay::Code:
      return appendToken(u"unit-width-iso-code");
      break;
    case NumberFormatOptions::CurrencyDisplay::Name:
      return appendToken(u"unit-width-full-name");
      break;
    case NumberFormatOptions::CurrencyDisplay::Symbol:
      // Default, no additional tokens needed.
      return true;
      break;
    case NumberFormatOptions::CurrencyDisplay::NarrowSymbol:
      return appendToken(u"unit-width-narrow");
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("unexpected currency display type");
      return false;
      break;
  }
}

static const MeasureUnit& FindSimpleMeasureUnit(std::string_view name) {
  const auto* measureUnit = std::lower_bound(
      std::begin(simpleMeasureUnits), std::end(simpleMeasureUnits), name,
      [](const auto& measureUnit, std::string_view name) {
        return name.compare(measureUnit.name) > 0;
      });
  MOZ_ASSERT(measureUnit != std::end(simpleMeasureUnits),
             "unexpected unit identifier: unit not found");
  MOZ_ASSERT(measureUnit->name == name,
             "unexpected unit identifier: wrong unit found");
  return *measureUnit;
}

static constexpr size_t MaxUnitLength() {
  size_t length = 0;
  for (const auto& unit : simpleMeasureUnits) {
    length = std::max(length, std::char_traits<char>::length(unit.name));
  }
  return length * 2 + std::char_traits<char>::length("-per-");
}

bool NumberFormatterSkeleton::unit(std::string_view unit) {
  MOZ_RELEASE_ASSERT(unit.length() <= MaxUnitLength());

  auto appendUnit = [this](const MeasureUnit& unit) {
    return append(unit.type, strlen(unit.type)) && append('-') &&
           append(unit.name, strlen(unit.name));
  };

  // |unit| can be a compound unit identifier, separated by "-per-".
  static constexpr char separator[] = "-per-";
  size_t separator_len = strlen(separator);
  size_t offset = unit.find(separator);
  if (offset != std::string_view::npos) {
    const auto& numerator = FindSimpleMeasureUnit(unit.substr(0, offset));
    const auto& denominator = FindSimpleMeasureUnit(
        std::string_view(unit.data() + offset + separator_len,
                         unit.length() - offset - separator_len));
    return append(u"measure-unit/") && appendUnit(numerator) && append(' ') &&
           append(u"per-measure-unit/") && appendUnit(denominator) &&
           append(' ');
  }

  const auto& simple = FindSimpleMeasureUnit(unit);
  return append(u"measure-unit/") && appendUnit(simple) && append(' ');
}

bool NumberFormatterSkeleton::unitDisplay(
    NumberFormatOptions::UnitDisplay display) {
  switch (display) {
    case NumberFormatOptions::UnitDisplay::Short:
      return appendToken(u"unit-width-short");
      break;
    case NumberFormatOptions::UnitDisplay::Narrow:
      return appendToken(u"unit-width-narrow");
      break;
    case NumberFormatOptions::UnitDisplay::Long:
      return appendToken(u"unit-width-full-name");
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("unexpected unit display type");
      return false;
      break;
  }
}

bool NumberFormatterSkeleton::percent() {
  return appendToken(u"percent scale/100");
}

bool NumberFormatterSkeleton::fractionDigits(uint32_t min, uint32_t max) {
  // Note: |min| can be zero here.
  MOZ_ASSERT(min <= max);
  return append('.') && appendN('0', min) && appendN('#', max - min) &&
         append(' ');
}

bool NumberFormatterSkeleton::minIntegerDigits(uint32_t min) {
  MOZ_ASSERT(min > 0);
  return append(u"integer-width/+") && appendN('0', min) && append(' ');
}

bool NumberFormatterSkeleton::significantDigits(uint32_t min, uint32_t max) {
  MOZ_ASSERT(min > 0);
  MOZ_ASSERT(min <= max);
  return appendN('@', min) && appendN('#', max - min) && append(' ');
}

bool NumberFormatterSkeleton::disableGrouping() {
  return appendToken(u"group-off");
}

bool NumberFormatterSkeleton::notation(NumberFormatOptions::Notation style) {
  switch (style) {
    case NumberFormatOptions::Notation::Standard:
      // Default, no additional tokens needed.
      return true;
      break;
    case NumberFormatOptions::Notation::Scientific:
      return appendToken(u"scientific");
      break;
    case NumberFormatOptions::Notation::Engineering:
      return appendToken(u"engineering");
      break;
    case NumberFormatOptions::Notation::CompactShort:
      return appendToken(u"compact-short");
      break;
    case NumberFormatOptions::Notation::CompactLong:
      return appendToken(u"compact-long");
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("unexpected notation style");
      return false;
      break;
  }
}

bool NumberFormatterSkeleton::signDisplay(
    NumberFormatOptions::SignDisplay display) {
  switch (display) {
    case NumberFormatOptions::SignDisplay::Auto:
      // Default, no additional tokens needed.
      return true;
      break;
    case NumberFormatOptions::SignDisplay::Always:
      return appendToken(u"sign-always");
      break;
    case NumberFormatOptions::SignDisplay::Never:
      return appendToken(u"sign-never");
      break;
    case NumberFormatOptions::SignDisplay::ExceptZero:
      return appendToken(u"sign-except-zero");
      break;
    case NumberFormatOptions::SignDisplay::Accounting:
      return appendToken(u"sign-accounting");
      break;
    case NumberFormatOptions::SignDisplay::AccountingAlways:
      return appendToken(u"sign-accounting-always");
      break;
    case NumberFormatOptions::SignDisplay::AccountingExceptZero:
      return appendToken(u"sign-accounting-except-zero");
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("unexpected sign display type");
      return false;
      break;
  }
}

bool NumberFormatterSkeleton::roundingModeHalfUp() {
  return appendToken(u"rounding-mode-half-up");
}

UNumberFormatter* NumberFormatterSkeleton::toFormatter(
    std::string_view locale) {
  if (!mValidSkeleton) {
    return nullptr;
  }

  UErrorCode status = U_ZERO_ERROR;
  UNumberFormatter* nf = unumf_openForSkeletonAndLocale(
      mVector.begin(), mVector.length(), locale.data(), &status);
  if (U_FAILURE(status)) {
    return nullptr;
  }
  return nf;
}

}  // namespace intl
}  // namespace mozilla
