// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/version.h"

#if OPENTELEMETRY_HAVE_WORKING_REGEX
#  include <regex>
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
class InstrumentMetaDataValidator
{
public:
  InstrumentMetaDataValidator();
  bool ValidateName(nostd::string_view name) const;
  bool ValidateUnit(nostd::string_view unit) const;
  bool ValidateDescription(nostd::string_view description) const;

private:
#if OPENTELEMETRY_HAVE_WORKING_REGEX
  const std::regex name_reg_key_;
  const std::regex unit_reg_key_;
#endif
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
