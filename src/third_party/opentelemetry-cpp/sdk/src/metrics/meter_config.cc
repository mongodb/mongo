// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/metrics/meter_config.h"
OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

OPENTELEMETRY_EXPORT bool MeterConfig::operator==(const MeterConfig &other) const noexcept
{
  return disabled_ == other.disabled_;
}

OPENTELEMETRY_EXPORT bool MeterConfig::IsEnabled() const noexcept
{
  return !disabled_;
}

OPENTELEMETRY_EXPORT MeterConfig MeterConfig::Disabled()
{
  static const auto kDisabledConfig = MeterConfig(true);
  return kDisabledConfig;
}

OPENTELEMETRY_EXPORT MeterConfig MeterConfig::Enabled()
{
  return Default();
}

OPENTELEMETRY_EXPORT MeterConfig MeterConfig::Default()
{
  static const auto kDefaultConfig = MeterConfig();
  return kDefaultConfig;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
