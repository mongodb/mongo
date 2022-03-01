/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef intl_components_NumberFormat_h_
#define intl_components_NumberFormat_h_
#include <string_view>
#include <utility>
#include <vector>

#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Result.h"
#include "mozilla/ResultVariant.h"
#include "mozilla/Utf8.h"
#include "mozilla/Vector.h"

#include "unicode/ustring.h"
#include "unicode/unum.h"
#include "unicode/unumberformatter.h"

struct UPluralRules;

namespace mozilla {
namespace intl {

struct PluralRulesOptions;

/**
 * Configure NumberFormat options.
 * The supported display styles are:
 *   * Decimal (default)
 *   * Currency (controlled by mCurrency)
 *   * Unit (controlled by mUnit)
 *   * Percent (controlled by mPercent)
 *
 * Only one of mCurrency, mUnit or mPercent should be set. If none are set,
 * the number will formatted as a decimal.
 *
 * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#unit
 */
struct MOZ_STACK_CLASS NumberFormatOptions {
  /**
   * Display a currency amount. |currency| must be a three-letter currency code.
   *
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#unit
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#unit-width
   */
  enum class CurrencyDisplay {
    Symbol,
    Code,
    Name,
    NarrowSymbol,
  };
  Maybe<std::pair<std::string_view, CurrencyDisplay>> mCurrency;

  /**
   * Set the fraction digits settings. |min| can be zero, |max| must be
   * larger-or-equal to |min|.
   *
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#fraction-precision
   */
  Maybe<std::pair<uint32_t, uint32_t>> mFractionDigits;

  /**
   * Set the minimum number of integer digits. |min| must be a non-zero
   * number.
   *
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#integer-width
   */
  Maybe<uint32_t> mMinIntegerDigits;

  /**
   * Set the significant digits settings. |min| must be a non-zero number, |max|
   * must be larger-or-equal to |min|.
   *
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#significant-digits-precision
   */
  Maybe<std::pair<uint32_t, uint32_t>> mSignificantDigits;

  /**
   * Display a unit amount. |unit| must be a well-formed unit identifier.
   *
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#unit
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#per-unit
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#unit-width
   */
  enum class UnitDisplay { Short, Narrow, Long };
  Maybe<std::pair<std::string_view, UnitDisplay>> mUnit;

  /**
   * Display a percent number.
   *
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#unit
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#scale
   */
  bool mPercent = false;

  /**
   * Enable or disable grouping.
   *
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#grouping
   */
  bool mUseGrouping = true;

  /**
   * Set the notation style.
   *
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#notation
   */
  enum class Notation {
    Standard,
    Scientific,
    Engineering,
    CompactShort,
    CompactLong
  } mNotation = Notation::Standard;

  /**
   * Set the sign-display.
   *
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#sign-display
   */
  enum class SignDisplay {
    Auto,
    Never,
    Always,
    ExceptZero,
    Accounting,
    AccountingAlways,
    AccountingExceptZero
  } mSignDisplay = SignDisplay::Auto;

  /**
   * Set the rounding mode to 'half-up'.
   *
   * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md#rounding-mode
   */
  bool mRoundingModeHalfUp = true;
};

enum class NumberPartType {
  Compact,
  Currency,
  Decimal,
  ExponentInteger,
  ExponentMinusSign,
  ExponentSeparator,
  Fraction,
  Group,
  Infinity,
  Integer,
  Literal,
  MinusSign,
  Nan,
  Percent,
  PlusSign,
  Unit,
};

// Because parts fully partition the formatted string, we only track the
// index of the end of each part -- the beginning is implicitly the last
// part's end.
using NumberPart = std::pair<NumberPartType, size_t>;

using NumberPartVector = mozilla::Vector<NumberPart, 8>;

/**
 * According to http://userguide.icu-project.org/design, as long as we constrain
 * ourselves to const APIs ICU is const-correct.
 */

/**
 * A NumberFormat implementation that roughly mirrors the API provided by
 * the ECMA-402 Intl.NumberFormat object.
 *
 * https://tc39.es/ecma402/#numberformat-objects
 */
class NumberFormat final {
 public:
  enum class FormatError {
    InternalError,
    OutOfMemory,
  };

  /**
   * Initialize a new NumberFormat for the provided locale and using the
   * provided options.
   *
   * https://tc39.es/ecma402/#sec-initializenumberformat
   */
  static Result<UniquePtr<NumberFormat>, NumberFormat::FormatError> TryCreate(
      std::string_view aLocale, const NumberFormatOptions& aOptions);

  NumberFormat() = default;
  NumberFormat(const NumberFormat&) = delete;
  NumberFormat& operator=(const NumberFormat&) = delete;
  ~NumberFormat();

  /**
   * Formats a double to a utf-16 string. The string view is valid until
   * another number is formatted. Accessing the string view after this event
   * is undefined behavior.
   *
   * https://tc39.es/ecma402/#sec-formatnumberstring
   */
  Result<std::u16string_view, NumberFormat::FormatError> format(
      double number) const {
    if (!formatInternal(number)) {
      return Err(FormatError::InternalError);
    }

    return formatResult();
  }

  /**
   * Formats a double to a utf-16 string, and fills the provided parts vector.
   * The string view is valid until another number is formatted. Accessing the
   * string view after this event is undefined behavior.
   *
   * https://tc39.es/ecma402/#sec-partitionnumberpattern
   */
  Result<std::u16string_view, NumberFormat::FormatError> formatToParts(
      double number, NumberPartVector& parts) const {
    if (!formatInternal(number)) {
      return Err(FormatError::InternalError);
    }

    bool isNegative = !IsNaN(number) && IsNegative(number);

    return formatResultToParts(Some(number), isNegative, parts);
  }

  /**
   * Formats a double to the provider buffer (either utf-8 or utf-16)
   *
   * https://tc39.es/ecma402/#sec-formatnumberstring
   */
  template <typename B>
  Result<Ok, NumberFormat::FormatError> format(double number, B& buffer) const {
    if (!formatInternal(number)) {
      return Err(FormatError::InternalError);
    }

    return formatResult<typename B::CharType, B>(buffer);
  }

  /**
   * Formats an int64_t to a utf-16 string. The string view is valid until
   * another number is formatted. Accessing the string view after this event is
   * undefined behavior.
   *
   * https://tc39.es/ecma402/#sec-formatnumberstring
   */
  Result<std::u16string_view, NumberFormat::FormatError> format(
      int64_t number) const {
    if (!formatInternal(number)) {
      return Err(FormatError::InternalError);
    }

    return formatResult();
  }

  /**
   * Formats a int64_t to a utf-16 string, and fills the provided parts vector.
   * The string view is valid until another number is formatted. Accessing the
   * string view after this event is undefined behavior.
   *
   * https://tc39.es/ecma402/#sec-partitionnumberpattern
   */
  Result<std::u16string_view, NumberFormat::FormatError> formatToParts(
      int64_t number, NumberPartVector& parts) const {
    if (!formatInternal(number)) {
      return Err(FormatError::InternalError);
    }

    return formatResultToParts(Nothing(), number < 0, parts);
  }

  /**
   * Formats an int64_t to the provider buffer (either utf-8 or utf-16).
   *
   * https://tc39.es/ecma402/#sec-formatnumberstring
   */
  template <typename B>
  Result<Ok, NumberFormat::FormatError> format(int64_t number,
                                               B& buffer) const {
    if (!formatInternal(number)) {
      return Err(FormatError::InternalError);
    }

    return formatResult<typename B::CharType, B>(buffer);
  }

  /**
   * Formats a string encoded big integer to a utf-16 string. The string view
   * is valid until another number is formatted. Accessing the string view
   * after this event is undefined behavior.
   *
   * https://tc39.es/ecma402/#sec-formatnumberstring
   */
  Result<std::u16string_view, NumberFormat::FormatError> format(
      std::string_view number) const {
    if (!formatInternal(number)) {
      return Err(FormatError::InternalError);
    }

    return formatResult();
  }

  /**
   * Formats a string encoded big integer to a utf-16 string, and fills the
   * provided parts vector. The string view is valid until another number is
   * formatted. Accessing the string view after this event is undefined
   * behavior.
   *
   * https://tc39.es/ecma402/#sec-partitionnumberpattern
   */
  Result<std::u16string_view, NumberFormat::FormatError> formatToParts(
      std::string_view number, NumberPartVector& parts) const {
    if (!formatInternal(number)) {
      return Err(FormatError::InternalError);
    }

    bool isNegative = !number.empty() && number[0] == '-';

    return formatResultToParts(Nothing(), isNegative, parts);
  }

  /**
   * Formats a string encoded big integer to the provider buffer
   * (either utf-8 or utf-16).
   *
   * https://tc39.es/ecma402/#sec-formatnumberstring
   */
  template <typename B>
  Result<Ok, NumberFormat::FormatError> format(std::string_view number,
                                               B& buffer) const {
    if (!formatInternal(number)) {
      return Err(FormatError::InternalError);
    }

    return formatResult<typename B::CharType, B>(buffer);
  }

  /**
   * Formats the number and selects the keyword by using a provided
   * UPluralRules object.
   *
   * https://tc39.es/ecma402/#sec-intl.pluralrules.prototype.select
   *
   * TODO(1713917) This is necessary because both PluralRules and
   * NumberFormat have a shared dependency on the raw UFormattedNumber
   * type. Once we transition to using ICU4X, the FFI calls should no
   * longer require such shared dependencies. At that time, this
   * functionality should be removed from NumberFormat and invoked
   * solely from PluralRules.
   */
  Result<int32_t, NumberFormat::FormatError> selectFormatted(
      double number, char16_t* keyword, int32_t keywordSize,
      UPluralRules* pluralRules) const;

 private:
  UNumberFormatter* mNumberFormatter = nullptr;
  UFormattedNumber* mFormattedNumber = nullptr;
  bool mFormatForUnit = false;

  Result<Ok, NumberFormat::FormatError> initialize(
      std::string_view aLocale, const NumberFormatOptions& aOptions);

  [[nodiscard]] bool formatInternal(double number) const;
  [[nodiscard]] bool formatInternal(int64_t number) const;
  [[nodiscard]] bool formatInternal(std::string_view number) const;

  Maybe<NumberPartType> GetPartTypeForNumberField(UNumberFormatFields fieldName,
                                                  Maybe<double> number,
                                                  bool isNegative) const;

  Result<std::u16string_view, NumberFormat::FormatError> formatResult() const;
  Result<std::u16string_view, NumberFormat::FormatError> formatResultToParts(
      const Maybe<double> number, bool isNegative,
      NumberPartVector& parts) const;

  template <typename C, typename B>
  Result<Ok, NumberFormat::FormatError> formatResult(B& buffer) const {
    // We only support buffers with uint8_t or char16_t for now.
    static_assert(std::is_same<C, uint8_t>::value ||
                  std::is_same<C, char16_t>::value);

    return formatResult().andThen([&buffer](std::u16string_view result)
                                      -> Result<Ok, NumberFormat::FormatError> {
      if constexpr (std::is_same<C, uint8_t>::value) {
        if (!FillUTF8Buffer(Span(result.data(), result.size()), buffer)) {
          return Err(FormatError::OutOfMemory);
        }
        return Ok();
      } else {
        // ICU provides APIs which accept a buffer, but they just copy from an
        // internal buffer behind the scenes anyway.
        if (!buffer.reserve(result.size())) {
          return Err(FormatError::OutOfMemory);
        }
        PodCopy(static_cast<char16_t*>(buffer.data()), result.data(),
                result.size());
        buffer.written(result.size());

        return Ok();
      }
    });
  }
};

}  // namespace intl
}  // namespace mozilla

#endif
