// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/metrics/instrument_metadata_validator.h"
#include "opentelemetry/version.h"

#if OPENTELEMETRY_HAVE_WORKING_REGEX
#  include <regex>
#else
#  include <algorithm>
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
#if OPENTELEMETRY_HAVE_WORKING_REGEX
// instrument-name = ALPHA 0*254 ("_" / "." / "-" / "/" / ALPHA / DIGIT)
const std::string kInstrumentNamePattern = "[a-zA-Z][-_./a-zA-Z0-9]{0,254}";
//
const std::string kInstrumentUnitPattern = "[\x01-\x7F]{0,63}";
// instrument-unit = It can have a maximum length of 63 ASCII chars
#endif

InstrumentMetaDataValidator::InstrumentMetaDataValidator()
#if OPENTELEMETRY_HAVE_WORKING_REGEX
    // clang-format off
    : name_reg_key_{kInstrumentNamePattern},
      unit_reg_key_{kInstrumentUnitPattern}
// clang-format on
#endif
{}

bool InstrumentMetaDataValidator::ValidateName(nostd::string_view name) const
{

#if OPENTELEMETRY_HAVE_WORKING_REGEX
  return std::regex_match(name.data(), name_reg_key_);
#else
  const size_t kMaxSize = 255;
  // size atmost 255 chars
  if (name.size() > kMaxSize)
  {
    return false;
  }
  // first char should be alpha
  if (!isalpha(name[0]))
  {
    return false;
  }
  // subsequent chars should be either of alphabets, digits, underscore,
  // minus, dot, slash
  return !std::any_of(std::next(name.begin()), name.end(), [](char c) {
    return !isalnum(c) && (c != '-') && (c != '_') && (c != '.') && (c != '/');
  });
#endif
}

bool InstrumentMetaDataValidator::ValidateUnit(nostd::string_view unit) const
{
#if OPENTELEMETRY_HAVE_WORKING_REGEX
  return std::regex_match(unit.data(), unit_reg_key_);
#else
  const size_t kMaxSize = 63;
  // length atmost 63 chars
  if (unit.size() > kMaxSize)
  {
    return false;
  }
  // all should be ascii chars.
  return !std::any_of(unit.begin(), unit.end(),
                      [](char c) { return static_cast<unsigned char>(c) > 127; });
#endif
}

bool InstrumentMetaDataValidator::ValidateDescription(nostd::string_view /*description*/) const
{
  return true;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
