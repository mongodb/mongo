// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

enum class PreferredAggregationTemporality
{
  kUnspecified,
  kDelta,
  kCumulative,
  kLowMemory,
};

}
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
