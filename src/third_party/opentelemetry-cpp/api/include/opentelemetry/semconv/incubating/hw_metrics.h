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
namespace hw
{

/**
  Remaining fraction of battery charge.
  <p>
  gauge
 */
static constexpr const char *kMetricHwBatteryCharge     = "hw.battery.charge";
static constexpr const char *descrMetricHwBatteryCharge = "Remaining fraction of battery charge.";
static constexpr const char *unitMetricHwBatteryCharge  = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwBatteryCharge(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwBatteryCharge, descrMetricHwBatteryCharge,
                                 unitMetricHwBatteryCharge);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwBatteryCharge(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwBatteryCharge, descrMetricHwBatteryCharge,
                                  unitMetricHwBatteryCharge);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwBatteryCharge(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwBatteryCharge, descrMetricHwBatteryCharge,
                                           unitMetricHwBatteryCharge);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwBatteryCharge(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwBatteryCharge, descrMetricHwBatteryCharge,
                                            unitMetricHwBatteryCharge);
}

/**
  Lower limit of battery charge fraction to ensure proper operation.
  <p>
  gauge
 */
static constexpr const char *kMetricHwBatteryChargeLimit = "hw.battery.charge.limit";
static constexpr const char *descrMetricHwBatteryChargeLimit =
    "Lower limit of battery charge fraction to ensure proper operation.";
static constexpr const char *unitMetricHwBatteryChargeLimit = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwBatteryChargeLimit(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwBatteryChargeLimit, descrMetricHwBatteryChargeLimit,
                                 unitMetricHwBatteryChargeLimit);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwBatteryChargeLimit(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwBatteryChargeLimit, descrMetricHwBatteryChargeLimit,
                                  unitMetricHwBatteryChargeLimit);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwBatteryChargeLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(
      kMetricHwBatteryChargeLimit, descrMetricHwBatteryChargeLimit, unitMetricHwBatteryChargeLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwBatteryChargeLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(
      kMetricHwBatteryChargeLimit, descrMetricHwBatteryChargeLimit, unitMetricHwBatteryChargeLimit);
}

/**
  Time left before battery is completely charged or discharged.
  <p>
  gauge
 */
static constexpr const char *kMetricHwBatteryTimeLeft = "hw.battery.time_left";
static constexpr const char *descrMetricHwBatteryTimeLeft =
    "Time left before battery is completely charged or discharged.";
static constexpr const char *unitMetricHwBatteryTimeLeft = "s";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwBatteryTimeLeft(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwBatteryTimeLeft, descrMetricHwBatteryTimeLeft,
                                 unitMetricHwBatteryTimeLeft);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwBatteryTimeLeft(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwBatteryTimeLeft, descrMetricHwBatteryTimeLeft,
                                  unitMetricHwBatteryTimeLeft);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwBatteryTimeLeft(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwBatteryTimeLeft, descrMetricHwBatteryTimeLeft,
                                           unitMetricHwBatteryTimeLeft);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwBatteryTimeLeft(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwBatteryTimeLeft, descrMetricHwBatteryTimeLeft,
                                            unitMetricHwBatteryTimeLeft);
}

/**
  CPU current frequency.
  <p>
  gauge
 */
static constexpr const char *kMetricHwCpuSpeed     = "hw.cpu.speed";
static constexpr const char *descrMetricHwCpuSpeed = "CPU current frequency.";
static constexpr const char *unitMetricHwCpuSpeed  = "Hz";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwCpuSpeed(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwCpuSpeed, descrMetricHwCpuSpeed, unitMetricHwCpuSpeed);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwCpuSpeed(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwCpuSpeed, descrMetricHwCpuSpeed, unitMetricHwCpuSpeed);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwCpuSpeed(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwCpuSpeed, descrMetricHwCpuSpeed,
                                           unitMetricHwCpuSpeed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwCpuSpeed(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwCpuSpeed, descrMetricHwCpuSpeed,
                                            unitMetricHwCpuSpeed);
}

/**
  CPU maximum frequency.
  <p>
  gauge
 */
static constexpr const char *kMetricHwCpuSpeedLimit     = "hw.cpu.speed.limit";
static constexpr const char *descrMetricHwCpuSpeedLimit = "CPU maximum frequency.";
static constexpr const char *unitMetricHwCpuSpeedLimit  = "Hz";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwCpuSpeedLimit(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwCpuSpeedLimit, descrMetricHwCpuSpeedLimit,
                                 unitMetricHwCpuSpeedLimit);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwCpuSpeedLimit(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwCpuSpeedLimit, descrMetricHwCpuSpeedLimit,
                                  unitMetricHwCpuSpeedLimit);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwCpuSpeedLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwCpuSpeedLimit, descrMetricHwCpuSpeedLimit,
                                           unitMetricHwCpuSpeedLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwCpuSpeedLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwCpuSpeedLimit, descrMetricHwCpuSpeedLimit,
                                            unitMetricHwCpuSpeedLimit);
}

/**
  Energy consumed by the component.
  <p>
  counter
 */
static constexpr const char *kMetricHwEnergy     = "hw.energy";
static constexpr const char *descrMetricHwEnergy = "Energy consumed by the component.";
static constexpr const char *unitMetricHwEnergy  = "J";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricHwEnergy(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricHwEnergy, descrMetricHwEnergy, unitMetricHwEnergy);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricHwEnergy(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricHwEnergy, descrMetricHwEnergy, unitMetricHwEnergy);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwEnergy(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricHwEnergy, descrMetricHwEnergy,
                                             unitMetricHwEnergy);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwEnergy(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricHwEnergy, descrMetricHwEnergy,
                                              unitMetricHwEnergy);
}

/**
  Number of errors encountered by the component.
  <p>
  counter
 */
static constexpr const char *kMetricHwErrors     = "hw.errors";
static constexpr const char *descrMetricHwErrors = "Number of errors encountered by the component.";
static constexpr const char *unitMetricHwErrors  = "{error}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricHwErrors(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricHwErrors, descrMetricHwErrors, unitMetricHwErrors);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricHwErrors(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricHwErrors, descrMetricHwErrors, unitMetricHwErrors);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwErrors(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricHwErrors, descrMetricHwErrors,
                                             unitMetricHwErrors);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwErrors(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricHwErrors, descrMetricHwErrors,
                                              unitMetricHwErrors);
}

/**
  Fan speed in revolutions per minute.
  <p>
  gauge
 */
static constexpr const char *kMetricHwFanSpeed     = "hw.fan.speed";
static constexpr const char *descrMetricHwFanSpeed = "Fan speed in revolutions per minute.";
static constexpr const char *unitMetricHwFanSpeed  = "rpm";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwFanSpeed(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwFanSpeed, descrMetricHwFanSpeed, unitMetricHwFanSpeed);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwFanSpeed(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwFanSpeed, descrMetricHwFanSpeed, unitMetricHwFanSpeed);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwFanSpeed(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwFanSpeed, descrMetricHwFanSpeed,
                                           unitMetricHwFanSpeed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwFanSpeed(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwFanSpeed, descrMetricHwFanSpeed,
                                            unitMetricHwFanSpeed);
}

/**
  Speed limit in rpm.
  <p>
  gauge
 */
static constexpr const char *kMetricHwFanSpeedLimit     = "hw.fan.speed.limit";
static constexpr const char *descrMetricHwFanSpeedLimit = "Speed limit in rpm.";
static constexpr const char *unitMetricHwFanSpeedLimit  = "rpm";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwFanSpeedLimit(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwFanSpeedLimit, descrMetricHwFanSpeedLimit,
                                 unitMetricHwFanSpeedLimit);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwFanSpeedLimit(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwFanSpeedLimit, descrMetricHwFanSpeedLimit,
                                  unitMetricHwFanSpeedLimit);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwFanSpeedLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwFanSpeedLimit, descrMetricHwFanSpeedLimit,
                                           unitMetricHwFanSpeedLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwFanSpeedLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwFanSpeedLimit, descrMetricHwFanSpeedLimit,
                                            unitMetricHwFanSpeedLimit);
}

/**
  Fan speed expressed as a fraction of its maximum speed.
  <p>
  gauge
 */
static constexpr const char *kMetricHwFanSpeedRatio = "hw.fan.speed_ratio";
static constexpr const char *descrMetricHwFanSpeedRatio =
    "Fan speed expressed as a fraction of its maximum speed.";
static constexpr const char *unitMetricHwFanSpeedRatio = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwFanSpeedRatio(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwFanSpeedRatio, descrMetricHwFanSpeedRatio,
                                 unitMetricHwFanSpeedRatio);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwFanSpeedRatio(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwFanSpeedRatio, descrMetricHwFanSpeedRatio,
                                  unitMetricHwFanSpeedRatio);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwFanSpeedRatio(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwFanSpeedRatio, descrMetricHwFanSpeedRatio,
                                           unitMetricHwFanSpeedRatio);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwFanSpeedRatio(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwFanSpeedRatio, descrMetricHwFanSpeedRatio,
                                            unitMetricHwFanSpeedRatio);
}

/**
  Received and transmitted bytes by the GPU.
  <p>
  counter
 */
static constexpr const char *kMetricHwGpuIo     = "hw.gpu.io";
static constexpr const char *descrMetricHwGpuIo = "Received and transmitted bytes by the GPU.";
static constexpr const char *unitMetricHwGpuIo  = "By";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricHwGpuIo(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricHwGpuIo, descrMetricHwGpuIo, unitMetricHwGpuIo);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricHwGpuIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricHwGpuIo, descrMetricHwGpuIo, unitMetricHwGpuIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwGpuIo(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricHwGpuIo, descrMetricHwGpuIo, unitMetricHwGpuIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwGpuIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricHwGpuIo, descrMetricHwGpuIo,
                                              unitMetricHwGpuIo);
}

/**
  Size of the GPU memory.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwGpuMemoryLimit     = "hw.gpu.memory.limit";
static constexpr const char *descrMetricHwGpuMemoryLimit = "Size of the GPU memory.";
static constexpr const char *unitMetricHwGpuMemoryLimit  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHwGpuMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwGpuMemoryLimit, descrMetricHwGpuMemoryLimit,
                                         unitMetricHwGpuMemoryLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHwGpuMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwGpuMemoryLimit, descrMetricHwGpuMemoryLimit,
                                          unitMetricHwGpuMemoryLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwGpuMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricHwGpuMemoryLimit, descrMetricHwGpuMemoryLimit, unitMetricHwGpuMemoryLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwGpuMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricHwGpuMemoryLimit, descrMetricHwGpuMemoryLimit, unitMetricHwGpuMemoryLimit);
}

/**
  GPU memory used.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwGpuMemoryUsage     = "hw.gpu.memory.usage";
static constexpr const char *descrMetricHwGpuMemoryUsage = "GPU memory used.";
static constexpr const char *unitMetricHwGpuMemoryUsage  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHwGpuMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwGpuMemoryUsage, descrMetricHwGpuMemoryUsage,
                                         unitMetricHwGpuMemoryUsage);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHwGpuMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwGpuMemoryUsage, descrMetricHwGpuMemoryUsage,
                                          unitMetricHwGpuMemoryUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwGpuMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricHwGpuMemoryUsage, descrMetricHwGpuMemoryUsage, unitMetricHwGpuMemoryUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwGpuMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricHwGpuMemoryUsage, descrMetricHwGpuMemoryUsage, unitMetricHwGpuMemoryUsage);
}

/**
  Fraction of GPU memory used.
  <p>
  gauge
 */
static constexpr const char *kMetricHwGpuMemoryUtilization     = "hw.gpu.memory.utilization";
static constexpr const char *descrMetricHwGpuMemoryUtilization = "Fraction of GPU memory used.";
static constexpr const char *unitMetricHwGpuMemoryUtilization  = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricHwGpuMemoryUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwGpuMemoryUtilization, descrMetricHwGpuMemoryUtilization,
                                 unitMetricHwGpuMemoryUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricHwGpuMemoryUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwGpuMemoryUtilization, descrMetricHwGpuMemoryUtilization,
                                  unitMetricHwGpuMemoryUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwGpuMemoryUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwGpuMemoryUtilization,
                                           descrMetricHwGpuMemoryUtilization,
                                           unitMetricHwGpuMemoryUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwGpuMemoryUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwGpuMemoryUtilization,
                                            descrMetricHwGpuMemoryUtilization,
                                            unitMetricHwGpuMemoryUtilization);
}

/**
  Fraction of time spent in a specific task.
  <p>
  gauge
 */
static constexpr const char *kMetricHwGpuUtilization = "hw.gpu.utilization";
static constexpr const char *descrMetricHwGpuUtilization =
    "Fraction of time spent in a specific task.";
static constexpr const char *unitMetricHwGpuUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwGpuUtilization(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwGpuUtilization, descrMetricHwGpuUtilization,
                                 unitMetricHwGpuUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwGpuUtilization(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwGpuUtilization, descrMetricHwGpuUtilization,
                                  unitMetricHwGpuUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwGpuUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwGpuUtilization, descrMetricHwGpuUtilization,
                                           unitMetricHwGpuUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwGpuUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwGpuUtilization, descrMetricHwGpuUtilization,
                                            unitMetricHwGpuUtilization);
}

/**
  Ambient (external) temperature of the physical host.
  <p>
  gauge
 */
static constexpr const char *kMetricHwHostAmbientTemperature = "hw.host.ambient_temperature";
static constexpr const char *descrMetricHwHostAmbientTemperature =
    "Ambient (external) temperature of the physical host.";
static constexpr const char *unitMetricHwHostAmbientTemperature = "Cel";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricHwHostAmbientTemperature(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwHostAmbientTemperature,
                                 descrMetricHwHostAmbientTemperature,
                                 unitMetricHwHostAmbientTemperature);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricHwHostAmbientTemperature(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwHostAmbientTemperature,
                                  descrMetricHwHostAmbientTemperature,
                                  unitMetricHwHostAmbientTemperature);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwHostAmbientTemperature(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwHostAmbientTemperature,
                                           descrMetricHwHostAmbientTemperature,
                                           unitMetricHwHostAmbientTemperature);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwHostAmbientTemperature(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwHostAmbientTemperature,
                                            descrMetricHwHostAmbientTemperature,
                                            unitMetricHwHostAmbientTemperature);
}

/**
  Total energy consumed by the entire physical host, in joules.
  <p>
  The overall energy usage of a host MUST be reported using the specific @code hw.host.energy
  @endcode and @code hw.host.power @endcode metrics <strong>only</strong>, instead of the generic
  @code hw.energy @endcode and @code hw.power @endcode described in the previous section, to prevent
  summing up overlapping values. <p> counter
 */
static constexpr const char *kMetricHwHostEnergy = "hw.host.energy";
static constexpr const char *descrMetricHwHostEnergy =
    "Total energy consumed by the entire physical host, in joules.";
static constexpr const char *unitMetricHwHostEnergy = "J";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricHwHostEnergy(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricHwHostEnergy, descrMetricHwHostEnergy,
                                    unitMetricHwHostEnergy);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricHwHostEnergy(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricHwHostEnergy, descrMetricHwHostEnergy,
                                    unitMetricHwHostEnergy);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwHostEnergy(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricHwHostEnergy, descrMetricHwHostEnergy,
                                             unitMetricHwHostEnergy);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwHostEnergy(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricHwHostEnergy, descrMetricHwHostEnergy,
                                              unitMetricHwHostEnergy);
}

/**
  By how many degrees Celsius the temperature of the physical host can be increased, before reaching
  a warning threshold on one of the internal sensors. <p> gauge
 */
static constexpr const char *kMetricHwHostHeatingMargin = "hw.host.heating_margin";
static constexpr const char *descrMetricHwHostHeatingMargin =
    "By how many degrees Celsius the temperature of the physical host can be increased, before reaching a warning threshold on one of the internal sensors.
    ";
    static constexpr const char *unitMetricHwHostHeatingMargin = "Cel";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwHostHeatingMargin(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwHostHeatingMargin, descrMetricHwHostHeatingMargin,
                                 unitMetricHwHostHeatingMargin);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwHostHeatingMargin(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwHostHeatingMargin, descrMetricHwHostHeatingMargin,
                                  unitMetricHwHostHeatingMargin);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwHostHeatingMargin(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(
      kMetricHwHostHeatingMargin, descrMetricHwHostHeatingMargin, unitMetricHwHostHeatingMargin);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwHostHeatingMargin(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(
      kMetricHwHostHeatingMargin, descrMetricHwHostHeatingMargin, unitMetricHwHostHeatingMargin);
}

/**
  Instantaneous power consumed by the entire physical host in Watts (@code hw.host.energy @endcode
  is preferred). <p> The overall energy usage of a host MUST be reported using the specific @code
  hw.host.energy @endcode and @code hw.host.power @endcode metrics <strong>only</strong>, instead of
  the generic @code hw.energy @endcode and @code hw.power @endcode described in the previous
  section, to prevent summing up overlapping values. <p> gauge
 */
static constexpr const char *kMetricHwHostPower = "hw.host.power";
static constexpr const char *descrMetricHwHostPower =
    "Instantaneous power consumed by the entire physical host in Watts (`hw.host.energy` is preferred).
    ";
    static constexpr const char *unitMetricHwHostPower = "W";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwHostPower(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwHostPower, descrMetricHwHostPower, unitMetricHwHostPower);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwHostPower(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwHostPower, descrMetricHwHostPower,
                                  unitMetricHwHostPower);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwHostPower(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwHostPower, descrMetricHwHostPower,
                                           unitMetricHwHostPower);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwHostPower(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwHostPower, descrMetricHwHostPower,
                                            unitMetricHwHostPower);
}

/**
  Size of the logical disk.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwLogicalDiskLimit     = "hw.logical_disk.limit";
static constexpr const char *descrMetricHwLogicalDiskLimit = "Size of the logical disk.";
static constexpr const char *unitMetricHwLogicalDiskLimit  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHwLogicalDiskLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwLogicalDiskLimit, descrMetricHwLogicalDiskLimit,
                                         unitMetricHwLogicalDiskLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHwLogicalDiskLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwLogicalDiskLimit, descrMetricHwLogicalDiskLimit,
                                          unitMetricHwLogicalDiskLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwLogicalDiskLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricHwLogicalDiskLimit, descrMetricHwLogicalDiskLimit, unitMetricHwLogicalDiskLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwLogicalDiskLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricHwLogicalDiskLimit, descrMetricHwLogicalDiskLimit, unitMetricHwLogicalDiskLimit);
}

/**
  Logical disk space usage.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwLogicalDiskUsage     = "hw.logical_disk.usage";
static constexpr const char *descrMetricHwLogicalDiskUsage = "Logical disk space usage.";
static constexpr const char *unitMetricHwLogicalDiskUsage  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHwLogicalDiskUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwLogicalDiskUsage, descrMetricHwLogicalDiskUsage,
                                         unitMetricHwLogicalDiskUsage);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHwLogicalDiskUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwLogicalDiskUsage, descrMetricHwLogicalDiskUsage,
                                          unitMetricHwLogicalDiskUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwLogicalDiskUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricHwLogicalDiskUsage, descrMetricHwLogicalDiskUsage, unitMetricHwLogicalDiskUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwLogicalDiskUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricHwLogicalDiskUsage, descrMetricHwLogicalDiskUsage, unitMetricHwLogicalDiskUsage);
}

/**
  Logical disk space utilization as a fraction.
  <p>
  gauge
 */
static constexpr const char *kMetricHwLogicalDiskUtilization = "hw.logical_disk.utilization";
static constexpr const char *descrMetricHwLogicalDiskUtilization =
    "Logical disk space utilization as a fraction.";
static constexpr const char *unitMetricHwLogicalDiskUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricHwLogicalDiskUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwLogicalDiskUtilization,
                                 descrMetricHwLogicalDiskUtilization,
                                 unitMetricHwLogicalDiskUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricHwLogicalDiskUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwLogicalDiskUtilization,
                                  descrMetricHwLogicalDiskUtilization,
                                  unitMetricHwLogicalDiskUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwLogicalDiskUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwLogicalDiskUtilization,
                                           descrMetricHwLogicalDiskUtilization,
                                           unitMetricHwLogicalDiskUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwLogicalDiskUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwLogicalDiskUtilization,
                                            descrMetricHwLogicalDiskUtilization,
                                            unitMetricHwLogicalDiskUtilization);
}

/**
  Size of the memory module.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwMemorySize     = "hw.memory.size";
static constexpr const char *descrMetricHwMemorySize = "Size of the memory module.";
static constexpr const char *unitMetricHwMemorySize  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>> CreateSyncInt64MetricHwMemorySize(
    metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwMemorySize, descrMetricHwMemorySize,
                                         unitMetricHwMemorySize);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>> CreateSyncDoubleMetricHwMemorySize(
    metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwMemorySize, descrMetricHwMemorySize,
                                          unitMetricHwMemorySize);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwMemorySize(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricHwMemorySize, descrMetricHwMemorySize,
                                                   unitMetricHwMemorySize);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwMemorySize(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricHwMemorySize, descrMetricHwMemorySize,
                                                    unitMetricHwMemorySize);
}

/**
  Link speed.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwNetworkBandwidthLimit     = "hw.network.bandwidth.limit";
static constexpr const char *descrMetricHwNetworkBandwidthLimit = "Link speed.";
static constexpr const char *unitMetricHwNetworkBandwidthLimit  = "By/s";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHwNetworkBandwidthLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwNetworkBandwidthLimit,
                                         descrMetricHwNetworkBandwidthLimit,
                                         unitMetricHwNetworkBandwidthLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHwNetworkBandwidthLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwNetworkBandwidthLimit,
                                          descrMetricHwNetworkBandwidthLimit,
                                          unitMetricHwNetworkBandwidthLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwNetworkBandwidthLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricHwNetworkBandwidthLimit,
                                                   descrMetricHwNetworkBandwidthLimit,
                                                   unitMetricHwNetworkBandwidthLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwNetworkBandwidthLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricHwNetworkBandwidthLimit,
                                                    descrMetricHwNetworkBandwidthLimit,
                                                    unitMetricHwNetworkBandwidthLimit);
}

/**
  Utilization of the network bandwidth as a fraction.
  <p>
  gauge
 */
static constexpr const char *kMetricHwNetworkBandwidthUtilization =
    "hw.network.bandwidth.utilization";
static constexpr const char *descrMetricHwNetworkBandwidthUtilization =
    "Utilization of the network bandwidth as a fraction.";
static constexpr const char *unitMetricHwNetworkBandwidthUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricHwNetworkBandwidthUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwNetworkBandwidthUtilization,
                                 descrMetricHwNetworkBandwidthUtilization,
                                 unitMetricHwNetworkBandwidthUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricHwNetworkBandwidthUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwNetworkBandwidthUtilization,
                                  descrMetricHwNetworkBandwidthUtilization,
                                  unitMetricHwNetworkBandwidthUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwNetworkBandwidthUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwNetworkBandwidthUtilization,
                                           descrMetricHwNetworkBandwidthUtilization,
                                           unitMetricHwNetworkBandwidthUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwNetworkBandwidthUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwNetworkBandwidthUtilization,
                                            descrMetricHwNetworkBandwidthUtilization,
                                            unitMetricHwNetworkBandwidthUtilization);
}

/**
  Received and transmitted network traffic in bytes.
  <p>
  counter
 */
static constexpr const char *kMetricHwNetworkIo = "hw.network.io";
static constexpr const char *descrMetricHwNetworkIo =
    "Received and transmitted network traffic in bytes.";
static constexpr const char *unitMetricHwNetworkIo = "By";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricHwNetworkIo(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricHwNetworkIo, descrMetricHwNetworkIo,
                                    unitMetricHwNetworkIo);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricHwNetworkIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricHwNetworkIo, descrMetricHwNetworkIo,
                                    unitMetricHwNetworkIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwNetworkIo(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricHwNetworkIo, descrMetricHwNetworkIo,
                                             unitMetricHwNetworkIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwNetworkIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricHwNetworkIo, descrMetricHwNetworkIo,
                                              unitMetricHwNetworkIo);
}

/**
  Received and transmitted network traffic in packets (or frames).
  <p>
  counter
 */
static constexpr const char *kMetricHwNetworkPackets = "hw.network.packets";
static constexpr const char *descrMetricHwNetworkPackets =
    "Received and transmitted network traffic in packets (or frames).";
static constexpr const char *unitMetricHwNetworkPackets = "{packet}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricHwNetworkPackets(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricHwNetworkPackets, descrMetricHwNetworkPackets,
                                    unitMetricHwNetworkPackets);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricHwNetworkPackets(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricHwNetworkPackets, descrMetricHwNetworkPackets,
                                    unitMetricHwNetworkPackets);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwNetworkPackets(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricHwNetworkPackets, descrMetricHwNetworkPackets,
                                             unitMetricHwNetworkPackets);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwNetworkPackets(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricHwNetworkPackets, descrMetricHwNetworkPackets,
                                              unitMetricHwNetworkPackets);
}

/**
  Link status: @code 1 @endcode (up) or @code 0 @endcode (down).
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwNetworkUp     = "hw.network.up";
static constexpr const char *descrMetricHwNetworkUp = "Link status: `1` (up) or `0` (down).";
static constexpr const char *unitMetricHwNetworkUp  = "1";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>> CreateSyncInt64MetricHwNetworkUp(
    metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwNetworkUp, descrMetricHwNetworkUp,
                                         unitMetricHwNetworkUp);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>> CreateSyncDoubleMetricHwNetworkUp(
    metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwNetworkUp, descrMetricHwNetworkUp,
                                          unitMetricHwNetworkUp);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwNetworkUp(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricHwNetworkUp, descrMetricHwNetworkUp,
                                                   unitMetricHwNetworkUp);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwNetworkUp(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricHwNetworkUp, descrMetricHwNetworkUp,
                                                    unitMetricHwNetworkUp);
}

/**
  Endurance remaining for this SSD disk.
  <p>
  gauge
 */
static constexpr const char *kMetricHwPhysicalDiskEnduranceUtilization =
    "hw.physical_disk.endurance_utilization";
static constexpr const char *descrMetricHwPhysicalDiskEnduranceUtilization =
    "Endurance remaining for this SSD disk.";
static constexpr const char *unitMetricHwPhysicalDiskEnduranceUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricHwPhysicalDiskEnduranceUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwPhysicalDiskEnduranceUtilization,
                                 descrMetricHwPhysicalDiskEnduranceUtilization,
                                 unitMetricHwPhysicalDiskEnduranceUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricHwPhysicalDiskEnduranceUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwPhysicalDiskEnduranceUtilization,
                                  descrMetricHwPhysicalDiskEnduranceUtilization,
                                  unitMetricHwPhysicalDiskEnduranceUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwPhysicalDiskEnduranceUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwPhysicalDiskEnduranceUtilization,
                                           descrMetricHwPhysicalDiskEnduranceUtilization,
                                           unitMetricHwPhysicalDiskEnduranceUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwPhysicalDiskEnduranceUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwPhysicalDiskEnduranceUtilization,
                                            descrMetricHwPhysicalDiskEnduranceUtilization,
                                            unitMetricHwPhysicalDiskEnduranceUtilization);
}

/**
  Size of the disk.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwPhysicalDiskSize     = "hw.physical_disk.size";
static constexpr const char *descrMetricHwPhysicalDiskSize = "Size of the disk.";
static constexpr const char *unitMetricHwPhysicalDiskSize  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHwPhysicalDiskSize(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwPhysicalDiskSize, descrMetricHwPhysicalDiskSize,
                                         unitMetricHwPhysicalDiskSize);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHwPhysicalDiskSize(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwPhysicalDiskSize, descrMetricHwPhysicalDiskSize,
                                          unitMetricHwPhysicalDiskSize);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwPhysicalDiskSize(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricHwPhysicalDiskSize, descrMetricHwPhysicalDiskSize, unitMetricHwPhysicalDiskSize);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwPhysicalDiskSize(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricHwPhysicalDiskSize, descrMetricHwPhysicalDiskSize, unitMetricHwPhysicalDiskSize);
}

/**
  Value of the corresponding <a href="https://wikipedia.org/wiki/S.M.A.R.T.">S.M.A.R.T.</a>
  (Self-Monitoring, Analysis, and Reporting Technology) attribute. <p> gauge
 */
static constexpr const char *kMetricHwPhysicalDiskSmart = "hw.physical_disk.smart";
static constexpr const char *descrMetricHwPhysicalDiskSmart =
    "Value of the corresponding [S.M.A.R.T.](https://wikipedia.org/wiki/S.M.A.R.T.) "
    "(Self-Monitoring, Analysis, and Reporting Technology) attribute.";
static constexpr const char *unitMetricHwPhysicalDiskSmart = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwPhysicalDiskSmart(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwPhysicalDiskSmart, descrMetricHwPhysicalDiskSmart,
                                 unitMetricHwPhysicalDiskSmart);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwPhysicalDiskSmart(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwPhysicalDiskSmart, descrMetricHwPhysicalDiskSmart,
                                  unitMetricHwPhysicalDiskSmart);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwPhysicalDiskSmart(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(
      kMetricHwPhysicalDiskSmart, descrMetricHwPhysicalDiskSmart, unitMetricHwPhysicalDiskSmart);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwPhysicalDiskSmart(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(
      kMetricHwPhysicalDiskSmart, descrMetricHwPhysicalDiskSmart, unitMetricHwPhysicalDiskSmart);
}

/**
  Instantaneous power consumed by the component.
  <p>
  It is recommended to report @code hw.energy @endcode instead of @code hw.power @endcode when
  possible. <p> gauge
 */
static constexpr const char *kMetricHwPower     = "hw.power";
static constexpr const char *descrMetricHwPower = "Instantaneous power consumed by the component.";
static constexpr const char *unitMetricHwPower  = "W";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwPower(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwPower, descrMetricHwPower, unitMetricHwPower);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwPower(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwPower, descrMetricHwPower, unitMetricHwPower);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwPower(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwPower, descrMetricHwPower, unitMetricHwPower);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwPower(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwPower, descrMetricHwPower, unitMetricHwPower);
}

/**
  Maximum power output of the power supply.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwPowerSupplyLimit = "hw.power_supply.limit";
static constexpr const char *descrMetricHwPowerSupplyLimit =
    "Maximum power output of the power supply.";
static constexpr const char *unitMetricHwPowerSupplyLimit = "W";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHwPowerSupplyLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwPowerSupplyLimit, descrMetricHwPowerSupplyLimit,
                                         unitMetricHwPowerSupplyLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHwPowerSupplyLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwPowerSupplyLimit, descrMetricHwPowerSupplyLimit,
                                          unitMetricHwPowerSupplyLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwPowerSupplyLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricHwPowerSupplyLimit, descrMetricHwPowerSupplyLimit, unitMetricHwPowerSupplyLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwPowerSupplyLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricHwPowerSupplyLimit, descrMetricHwPowerSupplyLimit, unitMetricHwPowerSupplyLimit);
}

/**
  Current power output of the power supply.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwPowerSupplyUsage = "hw.power_supply.usage";
static constexpr const char *descrMetricHwPowerSupplyUsage =
    "Current power output of the power supply.";
static constexpr const char *unitMetricHwPowerSupplyUsage = "W";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricHwPowerSupplyUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwPowerSupplyUsage, descrMetricHwPowerSupplyUsage,
                                         unitMetricHwPowerSupplyUsage);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricHwPowerSupplyUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwPowerSupplyUsage, descrMetricHwPowerSupplyUsage,
                                          unitMetricHwPowerSupplyUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwPowerSupplyUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricHwPowerSupplyUsage, descrMetricHwPowerSupplyUsage, unitMetricHwPowerSupplyUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwPowerSupplyUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricHwPowerSupplyUsage, descrMetricHwPowerSupplyUsage, unitMetricHwPowerSupplyUsage);
}

/**
  Utilization of the power supply as a fraction of its maximum output.
  <p>
  gauge
 */
static constexpr const char *kMetricHwPowerSupplyUtilization = "hw.power_supply.utilization";
static constexpr const char *descrMetricHwPowerSupplyUtilization =
    "Utilization of the power supply as a fraction of its maximum output.";
static constexpr const char *unitMetricHwPowerSupplyUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricHwPowerSupplyUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwPowerSupplyUtilization,
                                 descrMetricHwPowerSupplyUtilization,
                                 unitMetricHwPowerSupplyUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricHwPowerSupplyUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwPowerSupplyUtilization,
                                  descrMetricHwPowerSupplyUtilization,
                                  unitMetricHwPowerSupplyUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwPowerSupplyUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwPowerSupplyUtilization,
                                           descrMetricHwPowerSupplyUtilization,
                                           unitMetricHwPowerSupplyUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwPowerSupplyUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwPowerSupplyUtilization,
                                            descrMetricHwPowerSupplyUtilization,
                                            unitMetricHwPowerSupplyUtilization);
}

/**
  Operational status: @code 1 @endcode (true) or @code 0 @endcode (false) for each of the possible
  states. <p>
  @code hw.status @endcode is currently specified as an <em>UpDownCounter</em> but would ideally be
  represented using a <a
  href="https://github.com/prometheus/OpenMetrics/blob/v1.0.0/specification/OpenMetrics.md#stateset"><em>StateSet</em>
  as defined in OpenMetrics</a>. This semantic convention will be updated once <em>StateSet</em> is
  specified in OpenTelemetry. This planned change is not expected to have any consequence on the way
  users query their timeseries backend to retrieve the values of @code hw.status @endcode over time.
  <p>
  updowncounter
 */
static constexpr const char *kMetricHwStatus = "hw.status";
static constexpr const char *descrMetricHwStatus =
    "Operational status: `1` (true) or `0` (false) for each of the possible states.";
static constexpr const char *unitMetricHwStatus = "1";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>> CreateSyncInt64MetricHwStatus(
    metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricHwStatus, descrMetricHwStatus, unitMetricHwStatus);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>> CreateSyncDoubleMetricHwStatus(
    metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricHwStatus, descrMetricHwStatus, unitMetricHwStatus);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwStatus(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricHwStatus, descrMetricHwStatus,
                                                   unitMetricHwStatus);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwStatus(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricHwStatus, descrMetricHwStatus,
                                                    unitMetricHwStatus);
}

/**
  Operations performed by the tape drive.
  <p>
  counter
 */
static constexpr const char *kMetricHwTapeDriveOperations = "hw.tape_drive.operations";
static constexpr const char *descrMetricHwTapeDriveOperations =
    "Operations performed by the tape drive.";
static constexpr const char *unitMetricHwTapeDriveOperations = "{operation}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricHwTapeDriveOperations(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricHwTapeDriveOperations, descrMetricHwTapeDriveOperations,
                                    unitMetricHwTapeDriveOperations);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricHwTapeDriveOperations(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricHwTapeDriveOperations, descrMetricHwTapeDriveOperations,
                                    unitMetricHwTapeDriveOperations);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwTapeDriveOperations(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricHwTapeDriveOperations,
                                             descrMetricHwTapeDriveOperations,
                                             unitMetricHwTapeDriveOperations);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwTapeDriveOperations(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricHwTapeDriveOperations,
                                              descrMetricHwTapeDriveOperations,
                                              unitMetricHwTapeDriveOperations);
}

/**
  Temperature in degrees Celsius.
  <p>
  gauge
 */
static constexpr const char *kMetricHwTemperature     = "hw.temperature";
static constexpr const char *descrMetricHwTemperature = "Temperature in degrees Celsius.";
static constexpr const char *unitMetricHwTemperature  = "Cel";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwTemperature(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwTemperature, descrMetricHwTemperature,
                                 unitMetricHwTemperature);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwTemperature(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwTemperature, descrMetricHwTemperature,
                                  unitMetricHwTemperature);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwTemperature(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwTemperature, descrMetricHwTemperature,
                                           unitMetricHwTemperature);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwTemperature(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwTemperature, descrMetricHwTemperature,
                                            unitMetricHwTemperature);
}

/**
  Temperature limit in degrees Celsius.
  <p>
  gauge
 */
static constexpr const char *kMetricHwTemperatureLimit = "hw.temperature.limit";
static constexpr const char *descrMetricHwTemperatureLimit =
    "Temperature limit in degrees Celsius.";
static constexpr const char *unitMetricHwTemperatureLimit = "Cel";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwTemperatureLimit(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwTemperatureLimit, descrMetricHwTemperatureLimit,
                                 unitMetricHwTemperatureLimit);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwTemperatureLimit(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwTemperatureLimit, descrMetricHwTemperatureLimit,
                                  unitMetricHwTemperatureLimit);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwTemperatureLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwTemperatureLimit, descrMetricHwTemperatureLimit,
                                           unitMetricHwTemperatureLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwTemperatureLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(
      kMetricHwTemperatureLimit, descrMetricHwTemperatureLimit, unitMetricHwTemperatureLimit);
}

/**
  Voltage measured by the sensor.
  <p>
  gauge
 */
static constexpr const char *kMetricHwVoltage     = "hw.voltage";
static constexpr const char *descrMetricHwVoltage = "Voltage measured by the sensor.";
static constexpr const char *unitMetricHwVoltage  = "V";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwVoltage(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwVoltage, descrMetricHwVoltage, unitMetricHwVoltage);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwVoltage(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwVoltage, descrMetricHwVoltage, unitMetricHwVoltage);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwVoltage(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwVoltage, descrMetricHwVoltage,
                                           unitMetricHwVoltage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricHwVoltage(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwVoltage, descrMetricHwVoltage,
                                            unitMetricHwVoltage);
}

/**
  Voltage limit in Volts.
  <p>
  gauge
 */
static constexpr const char *kMetricHwVoltageLimit     = "hw.voltage.limit";
static constexpr const char *descrMetricHwVoltageLimit = "Voltage limit in Volts.";
static constexpr const char *unitMetricHwVoltageLimit  = "V";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwVoltageLimit(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwVoltageLimit, descrMetricHwVoltageLimit,
                                 unitMetricHwVoltageLimit);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwVoltageLimit(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwVoltageLimit, descrMetricHwVoltageLimit,
                                  unitMetricHwVoltageLimit);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricHwVoltageLimit(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwVoltageLimit, descrMetricHwVoltageLimit,
                                           unitMetricHwVoltageLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwVoltageLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwVoltageLimit, descrMetricHwVoltageLimit,
                                            unitMetricHwVoltageLimit);
}

/**
  Nominal (expected) voltage.
  <p>
  gauge
 */
static constexpr const char *kMetricHwVoltageNominal     = "hw.voltage.nominal";
static constexpr const char *descrMetricHwVoltageNominal = "Nominal (expected) voltage.";
static constexpr const char *unitMetricHwVoltageNominal  = "V";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricHwVoltageNominal(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricHwVoltageNominal, descrMetricHwVoltageNominal,
                                 unitMetricHwVoltageNominal);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricHwVoltageNominal(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricHwVoltageNominal, descrMetricHwVoltageNominal,
                                  unitMetricHwVoltageNominal);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricHwVoltageNominal(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricHwVoltageNominal, descrMetricHwVoltageNominal,
                                           unitMetricHwVoltageNominal);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricHwVoltageNominal(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricHwVoltageNominal, descrMetricHwVoltageNominal,
                                            unitMetricHwVoltageNominal);
}

}  // namespace hw
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
