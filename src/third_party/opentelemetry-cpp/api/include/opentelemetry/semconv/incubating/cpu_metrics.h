/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_metrics-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace cpu
{

/**
  Deprecated. Use @code system.cpu.frequency @endcode instead.

  @deprecated
  {"note": "Replaced by @code system.cpu.frequency @endcode.", "reason": "renamed", "renamed_to":
  "system.cpu.frequency"} <p> gauge
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricCpuFrequency = "cpu.frequency";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricCpuFrequency =
    "Deprecated. Use `system.cpu.frequency` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricCpuFrequency = "{Hz}";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricCpuFrequency(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricCpuFrequency, descrMetricCpuFrequency,
                                 unitMetricCpuFrequency);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricCpuFrequency(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricCpuFrequency, descrMetricCpuFrequency,
                                  unitMetricCpuFrequency);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricCpuFrequency(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricCpuFrequency, descrMetricCpuFrequency,
                                           unitMetricCpuFrequency);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricCpuFrequency(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricCpuFrequency, descrMetricCpuFrequency,
                                            unitMetricCpuFrequency);
}

/**
  Deprecated. Use @code system.cpu.time @endcode instead.

  @deprecated
  {"note": "Replaced by @code system.cpu.time @endcode.", "reason": "renamed", "renamed_to":
  "system.cpu.time"} <p> counter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricCpuTime = "cpu.time";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricCpuTime =
    "Deprecated. Use `system.cpu.time` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricCpuTime = "s";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricCpuTime(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricCpuTime, descrMetricCpuTime, unitMetricCpuTime);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricCpuTime(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricCpuTime, descrMetricCpuTime, unitMetricCpuTime);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricCpuTime(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricCpuTime, descrMetricCpuTime, unitMetricCpuTime);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricCpuTime(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricCpuTime, descrMetricCpuTime,
                                              unitMetricCpuTime);
}

/**
  Deprecated. Use @code system.cpu.utilization @endcode instead.

  @deprecated
  {"note": "Replaced by @code system.cpu.utilization @endcode.", "reason": "renamed", "renamed_to":
  "system.cpu.utilization"} <p> gauge
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricCpuUtilization = "cpu.utilization";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricCpuUtilization =
    "Deprecated. Use `system.cpu.utilization` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricCpuUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricCpuUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricCpuUtilization, descrMetricCpuUtilization,
                                 unitMetricCpuUtilization);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricCpuUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricCpuUtilization, descrMetricCpuUtilization,
                                  unitMetricCpuUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricCpuUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricCpuUtilization, descrMetricCpuUtilization,
                                           unitMetricCpuUtilization);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricCpuUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricCpuUtilization, descrMetricCpuUtilization,
                                            unitMetricCpuUtilization);
}

}  // namespace cpu
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
