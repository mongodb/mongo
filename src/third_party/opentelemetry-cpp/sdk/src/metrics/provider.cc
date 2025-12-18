// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/metrics/meter_provider.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/common/disabled.h"
#include "opentelemetry/sdk/metrics/provider.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

void Provider::SetMeterProvider(
    const nostd::shared_ptr<opentelemetry::metrics::MeterProvider> &mp) noexcept
{
  bool disabled = opentelemetry::sdk::common::GetSdkDisabled();

  if (!disabled)
  {
    opentelemetry::metrics::Provider::SetMeterProvider(mp);
  }
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
