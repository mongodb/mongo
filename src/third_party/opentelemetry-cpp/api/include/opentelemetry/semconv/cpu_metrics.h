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
 * Operating frequency of the logical CPU in Hertz.
 * <p>
 * gauge
 */
static constexpr const char *kMetricCpuFrequency = "cpu.frequency";
static constexpr const char *descrMetricCpuFrequency =
    "Operating frequency of the logical CPU in Hertz.";
static constexpr const char *unitMetricCpuFrequency = "Hz";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricCpuFrequency(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricCpuFrequency, descrMetricCpuFrequency,
                                 unitMetricCpuFrequency);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricCpuFrequency(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricCpuFrequency, descrMetricCpuFrequency,
                                  unitMetricCpuFrequency);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricCpuFrequency(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricCpuFrequency, descrMetricCpuFrequency,
                                           unitMetricCpuFrequency);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricCpuFrequency(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricCpuFrequency, descrMetricCpuFrequency,
                                            unitMetricCpuFrequency);
}

/**
 * Seconds each logical CPU spent on each mode
 * <p>
 * counter
 */
static constexpr const char *kMetricCpuTime     = "cpu.time";
static constexpr const char *descrMetricCpuTime = "Seconds each logical CPU spent on each mode";
static constexpr const char *unitMetricCpuTime  = "s";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricCpuTime, descrMetricCpuTime, unitMetricCpuTime);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricCpuTime, descrMetricCpuTime, unitMetricCpuTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricCpuTime, descrMetricCpuTime, unitMetricCpuTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricCpuTime, descrMetricCpuTime,
                                              unitMetricCpuTime);
}

/**
 * For each logical CPU, the utilization is calculated as the change in cumulative CPU time
 * (cpu.time) over a measurement interval, divided by the elapsed time. <p> gauge
 */
static constexpr const char *kMetricCpuUtilization = "cpu.utilization";
static constexpr const char *descrMetricCpuUtilization =
    "For each logical CPU, the utilization is calculated as the change in cumulative CPU time "
    "(cpu.time) over a measurement interval, divided by the elapsed time.";
static constexpr const char *unitMetricCpuUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricCpuUtilization(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricCpuUtilization, descrMetricCpuUtilization,
                                 unitMetricCpuUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricCpuUtilization(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricCpuUtilization, descrMetricCpuUtilization,
                                  unitMetricCpuUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricCpuUtilization(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricCpuUtilization, descrMetricCpuUtilization,
                                           unitMetricCpuUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricCpuUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricCpuUtilization, descrMetricCpuUtilization,
                                            unitMetricCpuUtilization);
}

}  // namespace cpu
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
