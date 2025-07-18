// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <vector>
#include "opentelemetry/common/macros.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/global_log_handler.h"

#if OPENTELEMETRY_HAVE_WORKING_REGEX
#  include <regex>
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
class Predicate
{
public:
  virtual ~Predicate()                                                        = default;
  virtual bool Match(opentelemetry::nostd::string_view string) const noexcept = 0;
};

class PatternPredicate : public Predicate
{
public:
  PatternPredicate(opentelemetry::nostd::string_view pattern) : reg_key_{pattern.data()} {}
  bool Match(opentelemetry::nostd::string_view str) const noexcept override
  {
#if OPENTELEMETRY_HAVE_WORKING_REGEX
    return std::regex_match(str.data(), reg_key_);
#else
    // TBD - Support regex match for GCC4.8
    OTEL_INTERNAL_LOG_ERROR(
        "PatternPredicate::Match - failed. std::regex not fully supported for this compiler.");
    return false;  // not supported
#endif
  }

private:
#if OPENTELEMETRY_HAVE_WORKING_REGEX
  std::regex reg_key_;
#else
  std::string reg_key_;
#endif
};

class ExactPredicate : public Predicate
{
public:
  ExactPredicate(opentelemetry::nostd::string_view pattern) : pattern_{pattern} {}
  bool Match(opentelemetry::nostd::string_view str) const noexcept override
  {
    if (pattern_ == str)
    {
      return true;
    }
    return false;
  }

private:
  std::string pattern_;
};

class MatchEverythingPattern : public Predicate
{
  bool Match(opentelemetry::nostd::string_view /* str */) const noexcept override { return true; }
};

class MatchNothingPattern : public Predicate
{
  bool Match(opentelemetry::nostd::string_view /* str */) const noexcept override { return false; }
};
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
