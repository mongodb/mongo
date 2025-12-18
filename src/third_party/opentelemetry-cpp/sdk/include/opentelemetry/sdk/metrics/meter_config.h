// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
/**
 * MeterConfig defines various configurable aspects of a Meter's behavior.
 * This class should not be used directly to configure a Meter's behavior, instead a
 * ScopeConfigurator should be used to compute the desired MeterConfig which can then be used to
 * configure a Meter.
 */
class OPENTELEMETRY_EXPORT MeterConfig
{
public:
  bool operator==(const MeterConfig &other) const noexcept;

  /**
   * Returns if the Meter is enabled or disabled. Meters are enabled by default.
   * @return a boolean indicating if the Meter is enabled. Defaults to true.
   */
  bool IsEnabled() const noexcept;

  /**
   * Returns a MeterConfig that represents a disabled Meter. A disabled meter behaves like a
   * no-op meter.
   * @return a static constant MeterConfig that represents a disabled meter.
   */
  static MeterConfig Disabled();

  /**
   * Returns a MeterConfig that represents an enabled Meter.
   * @return a static constant MeterConfig that represents an enabled meter.
   */
  static MeterConfig Enabled();

  /**
   * Returns a MeterConfig that represents a Meter configured with the default behavior.
   * The default behavior is guided by the OpenTelemetry specification.
   * @return a static constant MeterConfig that represents a meter configured with default
   * behavior.
   */
  static MeterConfig Default();

private:
  explicit MeterConfig(const bool disabled = false) : disabled_(disabled) {}
  bool disabled_;
};
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
