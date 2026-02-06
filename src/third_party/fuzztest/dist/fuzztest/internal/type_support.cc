// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./fuzztest/internal/type_support.h"

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "absl/numeric/int128.h"
#include "absl/strings/match.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "./fuzztest/internal/printer.h"

namespace fuzztest::internal {
namespace {

using ::fuzztest::domain_implementor::PrintMode;
using ::fuzztest::domain_implementor::RawSink;

template <typename T>
void PrintFloatingPointLiteral(T v, RawSink out, absl::string_view suffix) {
  // "%#.*gf" would get us a valid literal, but the result would be unreasonably
  // long. Unfortunately, both the retention of a decimal point and the
  // retention of trailing zeros are controlled by that flag for g, and that
  // format specifier is needed to reasonably choose between decimal and
  // exponential notation.
  std::string num_string =
      absl::StrFormat("%.*g", std::numeric_limits<T>::max_digits10, v);
  bool needs_decimal = std::isfinite(v) &&
                       !absl::StrContains(num_string, '.') &&
                       !absl::StrContains(num_string, 'e');
  bool needs_suffix = std::isfinite(v);
  absl::Format(out, "%s%s%s", num_string, needs_decimal ? "." : "",
               needs_suffix ? suffix : "");
}

}  // namespace

void IntegralPrinter::PrintUserValue(bool v, RawSink out, PrintMode) const {
  absl::Format(out, "%s", v ? "true" : "false");
}

void IntegralPrinter::PrintUserValue(absl::uint128 v, RawSink out,
                                     PrintMode) const {
  absl::Format(out, "%d", v);
}

void IntegralPrinter::PrintUserValue(absl::int128 v, RawSink out,
                                     PrintMode) const {
  absl::Format(out, "%d", v);
}

void IntegralPrinter::PrintUserValue(char v, RawSink out,
                                     PrintMode mode) const {
  auto unsigned_v = static_cast<uint8_t>(v);
  switch (mode) {
    case PrintMode::kHumanReadable:
      if (std::isprint(v)) {
        absl::Format(out, "'%c' (%d)", v, v);
      } else {
        absl::Format(out, "0x%02x (%u)", unsigned_v, unsigned_v);
      }
      break;
    case PrintMode::kSourceCode:
      if (std::isprint(v)) {
        absl::Format(out, "'%c'", v);
      } else {
        absl::Format(out, "'\\%03o'", unsigned_v);
      }
  }
}

static void PrintNonFinite(double v, absl::string_view nan,
                           absl::string_view type, RawSink out) {
  if (std::isnan(v)) {
    absl::Format(out, "std::%s(\"\")", nan);
  } else {
    assert(std::isinf(v));
    absl::Format(out, "%sstd::numeric_limits<%s>::infinity()", v > 0 ? "" : "-",
                 type);
  }
}

void FloatingPrinter::PrintUserValue(float v, RawSink out,
                                     PrintMode mode) const {
  if (mode == PrintMode::kSourceCode && !std::isfinite(v)) {
    return PrintNonFinite(v, "nanf", "float", out);
  }
  PrintFloatingPointLiteral(v, out, "f");
}

void FloatingPrinter::PrintUserValue(double v, RawSink out,
                                     PrintMode mode) const {
  if (mode == PrintMode::kSourceCode && !std::isfinite(v)) {
    return PrintNonFinite(v, "nan", "double", out);
  }
  PrintFloatingPointLiteral(v, out, "");
}

void FloatingPrinter::PrintUserValue(long double v, RawSink out,
                                     PrintMode mode) const {
  if (mode == PrintMode::kSourceCode && !std::isfinite(v)) {
    return PrintNonFinite(v, "nanl", "long double", out);
  }
  PrintFloatingPointLiteral(v, out, "L");
}

}  // namespace fuzztest::internal
