// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW

#  include <memory>

#  include "opentelemetry/sdk/metrics/state/filtered_ordered_attribute_map.h"
#  include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace context
{
class Context;
}  // namespace context

namespace sdk
{
namespace common
{
class OrderedAttributeMap;
}  // namespace common

namespace metrics
{
using MetricAttributes = opentelemetry::sdk::metrics::FilteredOrderedAttributeMap;

/**
 * Exemplar filter type is used to pre-filter measurements before attempting to store them in a
 * reservoir.
 * https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/metrics/sdk.md#exemplarfilter
 */
enum class ExemplarFilterType : uint8_t
{
  kAlwaysOff,
  kAlwaysOn,
  kTraceBased
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

#endif  // ENABLE_METRICS_EXEMPLAR_PREVIEW
