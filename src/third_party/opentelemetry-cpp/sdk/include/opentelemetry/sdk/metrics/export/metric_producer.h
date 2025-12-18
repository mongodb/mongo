// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/export/metric_filter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace resource
{
class Resource;
}  // namespace resource

namespace instrumentationscope
{
class InstrumentationScope;
}  // namespace instrumentationscope

namespace metrics
{

/**
 * Metric Data to be exported along with resources and
 * Instrumentation scope.
 */
struct ScopeMetrics
{
  const opentelemetry::sdk::instrumentationscope::InstrumentationScope *scope_ = nullptr;
  std::vector<MetricData> metric_data_;

  template <class ScopePtr, class MetricDataType>
  inline ScopeMetrics(ScopePtr &&scope, MetricDataType &&metric)
      : scope_{std::forward<ScopePtr>(scope)}, metric_data_{std::forward<MetricDataType>(metric)}
  {}

  inline ScopeMetrics() {}
  inline ScopeMetrics(const ScopeMetrics &) = default;
  inline ScopeMetrics(ScopeMetrics &&)      = default;

  inline ScopeMetrics &operator=(const ScopeMetrics &) = default;

  inline ScopeMetrics &operator=(ScopeMetrics &&) = default;
};

struct ResourceMetrics
{
  const opentelemetry::sdk::resource::Resource *resource_ = nullptr;
  std::vector<ScopeMetrics> scope_metric_data_;

  template <class ResourcePtr, class ScopeMetricsType>
  inline ResourceMetrics(ResourcePtr &&resource, ScopeMetricsType &&scope_metric_data)
      : resource_{std::forward<ResourcePtr>(resource)},
        scope_metric_data_{std::forward<ScopeMetricsType>(scope_metric_data)}
  {}

  inline ResourceMetrics() {}
  inline ResourceMetrics(const ResourceMetrics &) = default;
  inline ResourceMetrics(ResourceMetrics &&)      = default;

  inline ResourceMetrics &operator=(const ResourceMetrics &) = default;

  inline ResourceMetrics &operator=(ResourceMetrics &&) = default;
};

/**
 * MetricProducer defines the interface which bridges to third-party metric sources MUST implement,
 * so they can be plugged into an OpenTelemetry MetricReader as a source of aggregated metric data.
 *
 * Implementations must be thread-safe, and should accept configuration for the
 * AggregationTemporality of produced metrics.
 */
class MetricProducer
{
public:
  MetricProducer()          = default;
  virtual ~MetricProducer() = default;

  MetricProducer(const MetricProducer &)  = delete;
  MetricProducer(const MetricProducer &&) = delete;

  enum class Status
  {
    kSuccess,
    kFailure,
    kTimeout,
  };

  struct Result
  {
    ResourceMetrics points_;
    Status status_;
  };

  /**
   * Produce returns a batch of Metric Points, with a single instrumentation scope that identifies
   * the MetricProducer. Implementations may return successfully collected points even if there is a
   * partial failure.
   */
  virtual Result Produce() noexcept = 0;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
