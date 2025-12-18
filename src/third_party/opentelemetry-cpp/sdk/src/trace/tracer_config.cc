// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/trace/tracer_config.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

OPENTELEMETRY_EXPORT TracerConfig TracerConfig::Disabled()
{
  static const auto kDisabledConfig = TracerConfig(true);
  return kDisabledConfig;
}

OPENTELEMETRY_EXPORT TracerConfig TracerConfig::Enabled()
{
  return Default();
}

OPENTELEMETRY_EXPORT TracerConfig TracerConfig::Default()
{
  static const auto kDefaultConfig = TracerConfig();
  return kDefaultConfig;
}

OPENTELEMETRY_EXPORT bool TracerConfig::IsEnabled() const noexcept
{
  return !disabled_;
}

OPENTELEMETRY_EXPORT bool TracerConfig::operator==(const TracerConfig &other) const noexcept
{
  return disabled_ == other.disabled_;
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
