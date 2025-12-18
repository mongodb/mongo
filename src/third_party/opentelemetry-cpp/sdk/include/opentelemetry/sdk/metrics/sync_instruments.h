// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdint.h>
#include <memory>
#include <utility>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/context/context.h"
#include "opentelemetry/metrics/sync_instruments.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/state/metric_storage.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

class Synchronous
{
public:
  Synchronous(InstrumentDescriptor instrument_descriptor,
              std::unique_ptr<SyncWritableMetricStorage> storage)
      : instrument_descriptor_(instrument_descriptor), storage_(std::move(storage))
  {}

protected:
  InstrumentDescriptor instrument_descriptor_;
  std::unique_ptr<SyncWritableMetricStorage> storage_;
};

class LongCounter : public Synchronous, public opentelemetry::metrics::Counter<uint64_t>
{
public:
  LongCounter(const InstrumentDescriptor &instrument_descriptor,
              std::unique_ptr<SyncWritableMetricStorage> storage);

  void Add(uint64_t value,
           const opentelemetry::common::KeyValueIterable &attributes) noexcept override;

  void Add(uint64_t value,
           const opentelemetry::common::KeyValueIterable &attributes,
           const opentelemetry::context::Context &context) noexcept override;

  void Add(uint64_t value) noexcept override;

  void Add(uint64_t value, const opentelemetry::context::Context &context) noexcept override;
};

class DoubleCounter : public Synchronous, public opentelemetry::metrics::Counter<double>
{

public:
  DoubleCounter(const InstrumentDescriptor &instrument_descriptor,
                std::unique_ptr<SyncWritableMetricStorage> storage);

  void Add(double value,
           const opentelemetry::common::KeyValueIterable &attributes) noexcept override;
  void Add(double value,
           const opentelemetry::common::KeyValueIterable &attributes,
           const opentelemetry::context::Context &context) noexcept override;

  void Add(double value) noexcept override;
  void Add(double value, const opentelemetry::context::Context &context) noexcept override;
};

class LongUpDownCounter : public Synchronous, public opentelemetry::metrics::UpDownCounter<int64_t>
{
public:
  LongUpDownCounter(const InstrumentDescriptor &instrument_descriptor,
                    std::unique_ptr<SyncWritableMetricStorage> storage);

  void Add(int64_t value,
           const opentelemetry::common::KeyValueIterable &attributes) noexcept override;
  void Add(int64_t value,
           const opentelemetry::common::KeyValueIterable &attributes,
           const opentelemetry::context::Context &context) noexcept override;

  void Add(int64_t value) noexcept override;
  void Add(int64_t value, const opentelemetry::context::Context &context) noexcept override;
};

class DoubleUpDownCounter : public Synchronous, public opentelemetry::metrics::UpDownCounter<double>
{
public:
  DoubleUpDownCounter(const InstrumentDescriptor &instrument_descriptor,
                      std::unique_ptr<SyncWritableMetricStorage> storage);

  void Add(double value,
           const opentelemetry::common::KeyValueIterable &attributes) noexcept override;
  void Add(double value,
           const opentelemetry::common::KeyValueIterable &attributes,
           const opentelemetry::context::Context &context) noexcept override;

  void Add(double value) noexcept override;
  void Add(double value, const opentelemetry::context::Context &context) noexcept override;
};

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
class LongGauge : public Synchronous, public opentelemetry::metrics::Gauge<int64_t>
{
public:
  LongGauge(const InstrumentDescriptor &instrument_descriptor,
            std::unique_ptr<SyncWritableMetricStorage> storage);

  void Record(int64_t value,
              const opentelemetry::common::KeyValueIterable &attributes) noexcept override;
  void Record(int64_t value,
              const opentelemetry::common::KeyValueIterable &attributes,
              const opentelemetry::context::Context &context) noexcept override;

  void Record(int64_t value) noexcept override;
  void Record(int64_t value, const opentelemetry::context::Context &context) noexcept override;
};

class DoubleGauge : public Synchronous, public opentelemetry::metrics::Gauge<double>
{
public:
  DoubleGauge(const InstrumentDescriptor &instrument_descriptor,
              std::unique_ptr<SyncWritableMetricStorage> storage);

  void Record(double value,
              const opentelemetry::common::KeyValueIterable &attributes) noexcept override;
  void Record(double value,
              const opentelemetry::common::KeyValueIterable &attributes,
              const opentelemetry::context::Context &context) noexcept override;

  void Record(double value) noexcept override;
  void Record(double value, const opentelemetry::context::Context &context) noexcept override;
};
#endif

class LongHistogram : public Synchronous, public opentelemetry::metrics::Histogram<uint64_t>
{
public:
  LongHistogram(const InstrumentDescriptor &instrument_descriptor,
                std::unique_ptr<SyncWritableMetricStorage> storage);

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
  void Record(uint64_t value,
              const opentelemetry::common::KeyValueIterable &attributes) noexcept override;

  void Record(uint64_t value) noexcept override;
#endif

  void Record(uint64_t value,
              const opentelemetry::common::KeyValueIterable &attributes,
              const opentelemetry::context::Context &context) noexcept override;

  void Record(uint64_t value, const opentelemetry::context::Context &context) noexcept override;
};

class DoubleHistogram : public Synchronous, public opentelemetry::metrics::Histogram<double>
{
public:
  DoubleHistogram(const InstrumentDescriptor &instrument_descriptor,
                  std::unique_ptr<SyncWritableMetricStorage> storage);

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
  void Record(double value,
              const opentelemetry::common::KeyValueIterable &attributes) noexcept override;

  void Record(double value) noexcept override;
#endif

  void Record(double value,
              const opentelemetry::common::KeyValueIterable &attributes,
              const opentelemetry::context::Context &context) noexcept override;

  void Record(double value, const opentelemetry::context::Context &context) noexcept override;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
