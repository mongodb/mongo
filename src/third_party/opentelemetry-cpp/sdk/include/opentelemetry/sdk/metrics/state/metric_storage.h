// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <unordered_map>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/context/context.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/state/attributes_hashmap.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace common
{
class KeyValueIterable;
}  // namespace common

namespace sdk
{
namespace metrics
{

/* Represent the storage from which to collect the metrics */
class CollectorHandle;

class MetricStorage
{
public:
  MetricStorage()          = default;
  virtual ~MetricStorage() = default;

  /* collect the metrics from this storage */
  virtual bool Collect(CollectorHandle *collector,
                       nostd::span<std::shared_ptr<CollectorHandle>> collectors,
                       opentelemetry::common::SystemTimestamp sdk_start_ts,
                       opentelemetry::common::SystemTimestamp collection_ts,
                       nostd::function_ref<bool(MetricData)> callback) noexcept = 0;
};

/* Represents the sync metric storage */
class SyncWritableMetricStorage
{
public:
  virtual void RecordLong(int64_t value,
                          const opentelemetry::context::Context &context) noexcept = 0;

  virtual void RecordLong(int64_t value,
                          const opentelemetry::common::KeyValueIterable &attributes,
                          const opentelemetry::context::Context &context) noexcept = 0;

  virtual void RecordDouble(double value,
                            const opentelemetry::context::Context &context) noexcept = 0;

  virtual void RecordDouble(double value,
                            const opentelemetry::common::KeyValueIterable &attributes,
                            const opentelemetry::context::Context &context) noexcept = 0;

  virtual ~SyncWritableMetricStorage() = default;
};

/* Represents the async metric stroage */
class AsyncWritableMetricStorage
{
public:
  AsyncWritableMetricStorage()          = default;
  virtual ~AsyncWritableMetricStorage() = default;

  /* Records a batch of measurements */
  virtual void RecordLong(
      const std::unordered_map<MetricAttributes, int64_t, AttributeHashGenerator> &measurements,
      opentelemetry::common::SystemTimestamp observation_time) noexcept = 0;

  virtual void RecordDouble(
      const std::unordered_map<MetricAttributes, double, AttributeHashGenerator> &measurements,
      opentelemetry::common::SystemTimestamp observation_time) noexcept = 0;
};

class NoopMetricStorage : public MetricStorage
{
public:
  bool Collect(CollectorHandle * /* collector */,
               nostd::span<std::shared_ptr<CollectorHandle>> /* collectors */,
               opentelemetry::common::SystemTimestamp /* sdk_start_ts */,
               opentelemetry::common::SystemTimestamp /* collection_ts */,
               nostd::function_ref<bool(MetricData)> callback) noexcept override
  {
    MetricData metric_data{};
    return callback(std::move(metric_data));
  }
};

class NoopWritableMetricStorage : public SyncWritableMetricStorage
{
public:
  void RecordLong(int64_t value,
                  const opentelemetry::context::Context &context) noexcept override = 0;

  void RecordLong(int64_t /* value */,
                  const opentelemetry::common::KeyValueIterable & /* attributes */,
                  const opentelemetry::context::Context & /* context */) noexcept override
  {}

  void RecordDouble(double /* value */,
                    const opentelemetry::context::Context & /* context */) noexcept override
  {}

  void RecordDouble(double /* value */,
                    const opentelemetry::common::KeyValueIterable & /* attributes */,
                    const opentelemetry::context::Context & /* context */) noexcept override
  {}
};

class NoopAsyncWritableMetricStorage : public AsyncWritableMetricStorage
{
public:
  void RecordLong(const std::unordered_map<MetricAttributes, int64_t, AttributeHashGenerator>
                      & /* measurements */,
                  opentelemetry::common::SystemTimestamp /* observation_time */) noexcept override
  {}

  void RecordDouble(const std::unordered_map<MetricAttributes, double, AttributeHashGenerator>
                        & /* measurements */,
                    opentelemetry::common::SystemTimestamp /* observation_time */) noexcept override
  {}
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
