// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/trace/provider.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/common/disabled.h"
#include "opentelemetry/sdk/trace/provider.h"
#include "opentelemetry/trace/tracer_provider.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

void Provider::SetTracerProvider(
    const nostd::shared_ptr<opentelemetry::trace::TracerProvider> &tp) noexcept
{
  bool disabled = opentelemetry::sdk::common::GetSdkDisabled();

  if (!disabled)
  {
    opentelemetry::trace::Provider::SetTracerProvider(tp);
  }
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
