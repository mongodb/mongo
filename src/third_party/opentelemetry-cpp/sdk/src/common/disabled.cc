// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/common/disabled.h"
#include "opentelemetry/sdk/common/env_variables.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{

bool GetSdkDisabled()
{
  constexpr char kEnv[] = "OTEL_SDK_DISABLED";

  bool value{};

  const bool exists = GetBoolEnvironmentVariable(kEnv, value);
  if (!exists)
  {
    value = false;
  }

  return value;
}

}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
