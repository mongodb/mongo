/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef intl_components_NumberFormatterSkeleton_h_
#define intl_components_NumberFormatterSkeleton_h_
#include <string_view>
#include "mozilla/intl/NumberFormat.h"
#include "mozilla/Vector.h"
#include "unicode/unumberformatter.h"

namespace mozilla {
namespace intl {

/**
 * Class to create a number formatter skeleton.
 *
 * The skeleton syntax is documented at:
 * https://github.com/unicode-org/icu/blob/master/docs/userguide/format_parse/numbers/skeletons.md
 */
class MOZ_STACK_CLASS NumberFormatterSkeleton final {
 public:
  explicit NumberFormatterSkeleton(const NumberFormatOptions& options);

  /**
   * Return a new UNumberFormatter based on this skeleton.
   */
  UNumberFormatter* toFormatter(std::string_view locale);

 private:
  static constexpr size_t DefaultVectorSize = 128;

  mozilla::Vector<char16_t, DefaultVectorSize> mVector;
  bool mValidSkeleton = false;

  [[nodiscard]] bool append(char16_t c) { return mVector.append(c); }

  [[nodiscard]] bool appendN(char16_t c, size_t times) {
    return mVector.appendN(c, times);
  }

  template <size_t N>
  [[nodiscard]] bool append(const char16_t (&chars)[N]) {
    static_assert(N > 0,
                  "should only be used with string literals or properly "
                  "null-terminated arrays");
    MOZ_ASSERT(chars[N - 1] == '\0',
               "should only be used with string literals or properly "
               "null-terminated arrays");
    // Without trailing \0.
    return mVector.append(chars, N - 1);
  }

  template <size_t N>
  [[nodiscard]] bool appendToken(const char16_t (&token)[N]) {
    return append(token) && append(' ');
  }

  [[nodiscard]] bool append(const char* chars, size_t length) {
    return mVector.append(chars, length);
  }

  [[nodiscard]] bool currency(std::string_view currency);

  [[nodiscard]] bool currencyDisplay(
      NumberFormatOptions::CurrencyDisplay display);

  [[nodiscard]] bool unit(std::string_view unit);

  [[nodiscard]] bool unitDisplay(NumberFormatOptions::UnitDisplay display);

  [[nodiscard]] bool percent();

  [[nodiscard]] bool fractionDigits(uint32_t min, uint32_t max);

  [[nodiscard]] bool minIntegerDigits(uint32_t min);

  [[nodiscard]] bool significantDigits(uint32_t min, uint32_t max);

  [[nodiscard]] bool disableGrouping();

  [[nodiscard]] bool notation(NumberFormatOptions::Notation style);

  [[nodiscard]] bool signDisplay(NumberFormatOptions::SignDisplay display);

  [[nodiscard]] bool roundingModeHalfUp();
};

}  // namespace intl
}  // namespace mozilla

#endif
