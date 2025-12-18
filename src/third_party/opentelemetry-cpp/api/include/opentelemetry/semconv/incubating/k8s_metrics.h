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
namespace k8s
{

/**
  Maximum CPU resource limit set for the container.
  <p>
  See
  https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#resourcerequirements-v1-core
  for details. <p> updowncounter
 */
static constexpr const char *kMetricK8sContainerCpuLimit = "k8s.container.cpu.limit";
static constexpr const char *descrMetricK8sContainerCpuLimit =
    "Maximum CPU resource limit set for the container.";
static constexpr const char *unitMetricK8sContainerCpuLimit = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerCpuLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricK8sContainerCpuLimit, descrMetricK8sContainerCpuLimit, unitMetricK8sContainerCpuLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerCpuLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricK8sContainerCpuLimit, descrMetricK8sContainerCpuLimit, unitMetricK8sContainerCpuLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerCpuLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sContainerCpuLimit, descrMetricK8sContainerCpuLimit, unitMetricK8sContainerCpuLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerCpuLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sContainerCpuLimit, descrMetricK8sContainerCpuLimit, unitMetricK8sContainerCpuLimit);
}

/**
  The ratio of container CPU usage to its CPU limit.
  <p>
  The value range is [0.0,1.0]. A value of 1.0 means the container is using 100% of its CPU limit.
  If the CPU limit is not set, this metric SHOULD NOT be emitted for that container. <p> gauge
 */
static constexpr const char *kMetricK8sContainerCpuLimitUtilization =
    "k8s.container.cpu.limit_utilization";
static constexpr const char *descrMetricK8sContainerCpuLimitUtilization =
    "The ratio of container CPU usage to its CPU limit.";
static constexpr const char *unitMetricK8sContainerCpuLimitUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricK8sContainerCpuLimitUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sContainerCpuLimitUtilization,
                                 descrMetricK8sContainerCpuLimitUtilization,
                                 unitMetricK8sContainerCpuLimitUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricK8sContainerCpuLimitUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sContainerCpuLimitUtilization,
                                  descrMetricK8sContainerCpuLimitUtilization,
                                  unitMetricK8sContainerCpuLimitUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerCpuLimitUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sContainerCpuLimitUtilization,
                                           descrMetricK8sContainerCpuLimitUtilization,
                                           unitMetricK8sContainerCpuLimitUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerCpuLimitUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricK8sContainerCpuLimitUtilization,
                                            descrMetricK8sContainerCpuLimitUtilization,
                                            unitMetricK8sContainerCpuLimitUtilization);
}

/**
  CPU resource requested for the container.
  <p>
  See
  https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#resourcerequirements-v1-core
  for details. <p> updowncounter
 */
static constexpr const char *kMetricK8sContainerCpuRequest = "k8s.container.cpu.request";
static constexpr const char *descrMetricK8sContainerCpuRequest =
    "CPU resource requested for the container.";
static constexpr const char *unitMetricK8sContainerCpuRequest = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerCpuRequest(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerCpuRequest,
                                         descrMetricK8sContainerCpuRequest,
                                         unitMetricK8sContainerCpuRequest);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerCpuRequest(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerCpuRequest,
                                          descrMetricK8sContainerCpuRequest,
                                          unitMetricK8sContainerCpuRequest);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerCpuRequest(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sContainerCpuRequest,
                                                   descrMetricK8sContainerCpuRequest,
                                                   unitMetricK8sContainerCpuRequest);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerCpuRequest(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sContainerCpuRequest,
                                                    descrMetricK8sContainerCpuRequest,
                                                    unitMetricK8sContainerCpuRequest);
}

/**
  The ratio of container CPU usage to its CPU request.
  <p>
  gauge
 */
static constexpr const char *kMetricK8sContainerCpuRequestUtilization =
    "k8s.container.cpu.request_utilization";
static constexpr const char *descrMetricK8sContainerCpuRequestUtilization =
    "The ratio of container CPU usage to its CPU request.";
static constexpr const char *unitMetricK8sContainerCpuRequestUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricK8sContainerCpuRequestUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sContainerCpuRequestUtilization,
                                 descrMetricK8sContainerCpuRequestUtilization,
                                 unitMetricK8sContainerCpuRequestUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricK8sContainerCpuRequestUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sContainerCpuRequestUtilization,
                                  descrMetricK8sContainerCpuRequestUtilization,
                                  unitMetricK8sContainerCpuRequestUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerCpuRequestUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sContainerCpuRequestUtilization,
                                           descrMetricK8sContainerCpuRequestUtilization,
                                           unitMetricK8sContainerCpuRequestUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerCpuRequestUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricK8sContainerCpuRequestUtilization,
                                            descrMetricK8sContainerCpuRequestUtilization,
                                            unitMetricK8sContainerCpuRequestUtilization);
}

/**
  Maximum ephemeral storage resource limit set for the container.
  <p>
  See
  https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#resourcerequirements-v1-core
  for details. <p> updowncounter
 */
static constexpr const char *kMetricK8sContainerEphemeralStorageLimit =
    "k8s.container.ephemeral_storage.limit";
static constexpr const char *descrMetricK8sContainerEphemeralStorageLimit =
    "Maximum ephemeral storage resource limit set for the container.";
static constexpr const char *unitMetricK8sContainerEphemeralStorageLimit = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerEphemeralStorageLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerEphemeralStorageLimit,
                                         descrMetricK8sContainerEphemeralStorageLimit,
                                         unitMetricK8sContainerEphemeralStorageLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerEphemeralStorageLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerEphemeralStorageLimit,
                                          descrMetricK8sContainerEphemeralStorageLimit,
                                          unitMetricK8sContainerEphemeralStorageLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerEphemeralStorageLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sContainerEphemeralStorageLimit,
                                                   descrMetricK8sContainerEphemeralStorageLimit,
                                                   unitMetricK8sContainerEphemeralStorageLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerEphemeralStorageLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sContainerEphemeralStorageLimit,
                                                    descrMetricK8sContainerEphemeralStorageLimit,
                                                    unitMetricK8sContainerEphemeralStorageLimit);
}

/**
  Ephemeral storage resource requested for the container.
  <p>
  See
  https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#resourcerequirements-v1-core
  for details. <p> updowncounter
 */
static constexpr const char *kMetricK8sContainerEphemeralStorageRequest =
    "k8s.container.ephemeral_storage.request";
static constexpr const char *descrMetricK8sContainerEphemeralStorageRequest =
    "Ephemeral storage resource requested for the container.";
static constexpr const char *unitMetricK8sContainerEphemeralStorageRequest = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerEphemeralStorageRequest(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerEphemeralStorageRequest,
                                         descrMetricK8sContainerEphemeralStorageRequest,
                                         unitMetricK8sContainerEphemeralStorageRequest);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerEphemeralStorageRequest(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerEphemeralStorageRequest,
                                          descrMetricK8sContainerEphemeralStorageRequest,
                                          unitMetricK8sContainerEphemeralStorageRequest);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerEphemeralStorageRequest(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sContainerEphemeralStorageRequest,
                                                   descrMetricK8sContainerEphemeralStorageRequest,
                                                   unitMetricK8sContainerEphemeralStorageRequest);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerEphemeralStorageRequest(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sContainerEphemeralStorageRequest,
                                                    descrMetricK8sContainerEphemeralStorageRequest,
                                                    unitMetricK8sContainerEphemeralStorageRequest);
}

/**
  Maximum memory resource limit set for the container.
  <p>
  See
  https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#resourcerequirements-v1-core
  for details. <p> updowncounter
 */
static constexpr const char *kMetricK8sContainerMemoryLimit = "k8s.container.memory.limit";
static constexpr const char *descrMetricK8sContainerMemoryLimit =
    "Maximum memory resource limit set for the container.";
static constexpr const char *unitMetricK8sContainerMemoryLimit = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerMemoryLimit,
                                         descrMetricK8sContainerMemoryLimit,
                                         unitMetricK8sContainerMemoryLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerMemoryLimit,
                                          descrMetricK8sContainerMemoryLimit,
                                          unitMetricK8sContainerMemoryLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sContainerMemoryLimit,
                                                   descrMetricK8sContainerMemoryLimit,
                                                   unitMetricK8sContainerMemoryLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerMemoryLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sContainerMemoryLimit,
                                                    descrMetricK8sContainerMemoryLimit,
                                                    unitMetricK8sContainerMemoryLimit);
}

/**
  Memory resource requested for the container.
  <p>
  See
  https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#resourcerequirements-v1-core
  for details. <p> updowncounter
 */
static constexpr const char *kMetricK8sContainerMemoryRequest = "k8s.container.memory.request";
static constexpr const char *descrMetricK8sContainerMemoryRequest =
    "Memory resource requested for the container.";
static constexpr const char *unitMetricK8sContainerMemoryRequest = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerMemoryRequest(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerMemoryRequest,
                                         descrMetricK8sContainerMemoryRequest,
                                         unitMetricK8sContainerMemoryRequest);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerMemoryRequest(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerMemoryRequest,
                                          descrMetricK8sContainerMemoryRequest,
                                          unitMetricK8sContainerMemoryRequest);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerMemoryRequest(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sContainerMemoryRequest,
                                                   descrMetricK8sContainerMemoryRequest,
                                                   unitMetricK8sContainerMemoryRequest);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerMemoryRequest(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sContainerMemoryRequest,
                                                    descrMetricK8sContainerMemoryRequest,
                                                    unitMetricK8sContainerMemoryRequest);
}

/**
  Indicates whether the container is currently marked as ready to accept traffic, based on its
  readiness probe (1 = ready, 0 = not ready). <p> This metric SHOULD reflect the value of the @code
  ready @endcode field in the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#containerstatus-v1-core">K8s
  ContainerStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sContainerReady = "k8s.container.ready";
static constexpr const char *descrMetricK8sContainerReady =
    "Indicates whether the container is currently marked as ready to accept traffic, based on its readiness probe (1 = ready, 0 = not ready).
    ";
    static constexpr const char *unitMetricK8sContainerReady = "{container}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerReady(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerReady, descrMetricK8sContainerReady,
                                         unitMetricK8sContainerReady);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerReady(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerReady, descrMetricK8sContainerReady,
                                          unitMetricK8sContainerReady);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerReady(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sContainerReady, descrMetricK8sContainerReady, unitMetricK8sContainerReady);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerReady(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sContainerReady, descrMetricK8sContainerReady, unitMetricK8sContainerReady);
}

/**
  Describes how many times the container has restarted (since the last counter reset).
  <p>
  This value is pulled directly from the K8s API and the value can go indefinitely high and be reset
  to 0 at any time depending on how your kubelet is configured to prune dead containers. It is best
  to not depend too much on the exact value but rather look at it as either == 0, in which case you
  can conclude there were no restarts in the recent past, or > 0, in which case you can conclude
  there were restarts in the recent past, and not try and analyze the value beyond that. <p>
  updowncounter
 */
static constexpr const char *kMetricK8sContainerRestartCount = "k8s.container.restart.count";
static constexpr const char *descrMetricK8sContainerRestartCount =
    "Describes how many times the container has restarted (since the last counter reset).";
static constexpr const char *unitMetricK8sContainerRestartCount = "{restart}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerRestartCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerRestartCount,
                                         descrMetricK8sContainerRestartCount,
                                         unitMetricK8sContainerRestartCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerRestartCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerRestartCount,
                                          descrMetricK8sContainerRestartCount,
                                          unitMetricK8sContainerRestartCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerRestartCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sContainerRestartCount,
                                                   descrMetricK8sContainerRestartCount,
                                                   unitMetricK8sContainerRestartCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerRestartCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sContainerRestartCount,
                                                    descrMetricK8sContainerRestartCount,
                                                    unitMetricK8sContainerRestartCount);
}

/**
  Describes the number of K8s containers that are currently in a state for a given reason.
  <p>
  All possible container state reasons will be reported at each time interval to avoid missing
  metrics. Only the value corresponding to the current state reason will be non-zero. <p>
  updowncounter
 */
static constexpr const char *kMetricK8sContainerStatusReason = "k8s.container.status.reason";
static constexpr const char *descrMetricK8sContainerStatusReason =
    "Describes the number of K8s containers that are currently in a state for a given reason.";
static constexpr const char *unitMetricK8sContainerStatusReason = "{container}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerStatusReason(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerStatusReason,
                                         descrMetricK8sContainerStatusReason,
                                         unitMetricK8sContainerStatusReason);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerStatusReason(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerStatusReason,
                                          descrMetricK8sContainerStatusReason,
                                          unitMetricK8sContainerStatusReason);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerStatusReason(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sContainerStatusReason,
                                                   descrMetricK8sContainerStatusReason,
                                                   unitMetricK8sContainerStatusReason);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerStatusReason(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sContainerStatusReason,
                                                    descrMetricK8sContainerStatusReason,
                                                    unitMetricK8sContainerStatusReason);
}

/**
  Describes the number of K8s containers that are currently in a given state.
  <p>
  All possible container states will be reported at each time interval to avoid missing metrics.
  Only the value corresponding to the current state will be non-zero.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sContainerStatusState = "k8s.container.status.state";
static constexpr const char *descrMetricK8sContainerStatusState =
    "Describes the number of K8s containers that are currently in a given state.";
static constexpr const char *unitMetricK8sContainerStatusState = "{container}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerStatusState(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerStatusState,
                                         descrMetricK8sContainerStatusState,
                                         unitMetricK8sContainerStatusState);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerStatusState(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerStatusState,
                                          descrMetricK8sContainerStatusState,
                                          unitMetricK8sContainerStatusState);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerStatusState(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sContainerStatusState,
                                                   descrMetricK8sContainerStatusState,
                                                   unitMetricK8sContainerStatusState);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerStatusState(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sContainerStatusState,
                                                    descrMetricK8sContainerStatusState,
                                                    unitMetricK8sContainerStatusState);
}

/**
  Maximum storage resource limit set for the container.
  <p>
  See
  https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#resourcerequirements-v1-core
  for details. <p> updowncounter
 */
static constexpr const char *kMetricK8sContainerStorageLimit = "k8s.container.storage.limit";
static constexpr const char *descrMetricK8sContainerStorageLimit =
    "Maximum storage resource limit set for the container.";
static constexpr const char *unitMetricK8sContainerStorageLimit = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerStorageLimit(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerStorageLimit,
                                         descrMetricK8sContainerStorageLimit,
                                         unitMetricK8sContainerStorageLimit);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerStorageLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerStorageLimit,
                                          descrMetricK8sContainerStorageLimit,
                                          unitMetricK8sContainerStorageLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerStorageLimit(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sContainerStorageLimit,
                                                   descrMetricK8sContainerStorageLimit,
                                                   unitMetricK8sContainerStorageLimit);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerStorageLimit(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sContainerStorageLimit,
                                                    descrMetricK8sContainerStorageLimit,
                                                    unitMetricK8sContainerStorageLimit);
}

/**
  Storage resource requested for the container.
  <p>
  See
  https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#resourcerequirements-v1-core
  for details. <p> updowncounter
 */
static constexpr const char *kMetricK8sContainerStorageRequest = "k8s.container.storage.request";
static constexpr const char *descrMetricK8sContainerStorageRequest =
    "Storage resource requested for the container.";
static constexpr const char *unitMetricK8sContainerStorageRequest = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sContainerStorageRequest(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sContainerStorageRequest,
                                         descrMetricK8sContainerStorageRequest,
                                         unitMetricK8sContainerStorageRequest);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sContainerStorageRequest(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sContainerStorageRequest,
                                          descrMetricK8sContainerStorageRequest,
                                          unitMetricK8sContainerStorageRequest);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sContainerStorageRequest(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sContainerStorageRequest,
                                                   descrMetricK8sContainerStorageRequest,
                                                   unitMetricK8sContainerStorageRequest);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sContainerStorageRequest(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sContainerStorageRequest,
                                                    descrMetricK8sContainerStorageRequest,
                                                    unitMetricK8sContainerStorageRequest);
}

/**
  Deprecated, use @code k8s.cronjob.job.active @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.cronjob.job.active @endcode.", "reason": "renamed", "renamed_to":
  "k8s.cronjob.job.active"} <p> This metric aligns with the @code active @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#cronjobstatus-v1-batch">K8s
  CronJobStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sCronjobActiveJobs =
    "k8s.cronjob.active_jobs";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sCronjobActiveJobs =
    "Deprecated, use `k8s.cronjob.job.active` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sCronjobActiveJobs = "{job}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sCronjobActiveJobs(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricK8sCronjobActiveJobs, descrMetricK8sCronjobActiveJobs, unitMetricK8sCronjobActiveJobs);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sCronjobActiveJobs(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricK8sCronjobActiveJobs, descrMetricK8sCronjobActiveJobs, unitMetricK8sCronjobActiveJobs);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sCronjobActiveJobs(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sCronjobActiveJobs, descrMetricK8sCronjobActiveJobs, unitMetricK8sCronjobActiveJobs);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sCronjobActiveJobs(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sCronjobActiveJobs, descrMetricK8sCronjobActiveJobs, unitMetricK8sCronjobActiveJobs);
}

/**
  The number of actively running jobs for a cronjob.
  <p>
  This metric aligns with the @code active @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#cronjobstatus-v1-batch">K8s
  CronJobStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sCronjobJobActive = "k8s.cronjob.job.active";
static constexpr const char *descrMetricK8sCronjobJobActive =
    "The number of actively running jobs for a cronjob.";
static constexpr const char *unitMetricK8sCronjobJobActive = "{job}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sCronjobJobActive(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sCronjobJobActive, descrMetricK8sCronjobJobActive,
                                         unitMetricK8sCronjobJobActive);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sCronjobJobActive(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricK8sCronjobJobActive, descrMetricK8sCronjobJobActive, unitMetricK8sCronjobJobActive);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sCronjobJobActive(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sCronjobJobActive, descrMetricK8sCronjobJobActive, unitMetricK8sCronjobJobActive);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sCronjobJobActive(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sCronjobJobActive, descrMetricK8sCronjobJobActive, unitMetricK8sCronjobJobActive);
}

/**
  Deprecated, use @code k8s.daemonset.node.current_scheduled @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.daemonset.node.current_scheduled @endcode.", "reason": "renamed",
  "renamed_to": "k8s.daemonset.node.current_scheduled"} <p> This metric aligns with the @code
  currentNumberScheduled @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
  DaemonSetStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sDaemonsetCurrentScheduledNodes =
    "k8s.daemonset.current_scheduled_nodes";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sDaemonsetCurrentScheduledNodes =
    "Deprecated, use `k8s.daemonset.node.current_scheduled` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sDaemonsetCurrentScheduledNodes =
    "{node}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetCurrentScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetCurrentScheduledNodes,
                                         descrMetricK8sDaemonsetCurrentScheduledNodes,
                                         unitMetricK8sDaemonsetCurrentScheduledNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetCurrentScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetCurrentScheduledNodes,
                                          descrMetricK8sDaemonsetCurrentScheduledNodes,
                                          unitMetricK8sDaemonsetCurrentScheduledNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetCurrentScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetCurrentScheduledNodes,
                                                   descrMetricK8sDaemonsetCurrentScheduledNodes,
                                                   unitMetricK8sDaemonsetCurrentScheduledNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetCurrentScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetCurrentScheduledNodes,
                                                    descrMetricK8sDaemonsetCurrentScheduledNodes,
                                                    unitMetricK8sDaemonsetCurrentScheduledNodes);
}

/**
  Deprecated, use @code k8s.daemonset.node.desired_scheduled @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.daemonset.node.desired_scheduled @endcode.", "reason": "renamed",
  "renamed_to": "k8s.daemonset.node.desired_scheduled"} <p> This metric aligns with the @code
  desiredNumberScheduled @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
  DaemonSetStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sDaemonsetDesiredScheduledNodes =
    "k8s.daemonset.desired_scheduled_nodes";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sDaemonsetDesiredScheduledNodes =
    "Deprecated, use `k8s.daemonset.node.desired_scheduled` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sDaemonsetDesiredScheduledNodes =
    "{node}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetDesiredScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetDesiredScheduledNodes,
                                         descrMetricK8sDaemonsetDesiredScheduledNodes,
                                         unitMetricK8sDaemonsetDesiredScheduledNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetDesiredScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetDesiredScheduledNodes,
                                          descrMetricK8sDaemonsetDesiredScheduledNodes,
                                          unitMetricK8sDaemonsetDesiredScheduledNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetDesiredScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetDesiredScheduledNodes,
                                                   descrMetricK8sDaemonsetDesiredScheduledNodes,
                                                   unitMetricK8sDaemonsetDesiredScheduledNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetDesiredScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetDesiredScheduledNodes,
                                                    descrMetricK8sDaemonsetDesiredScheduledNodes,
                                                    unitMetricK8sDaemonsetDesiredScheduledNodes);
}

/**
  Deprecated, use @code k8s.daemonset.node.misscheduled @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.daemonset.node.misscheduled @endcode.", "reason": "renamed",
  "renamed_to": "k8s.daemonset.node.misscheduled"} <p> This metric aligns with the @code
  numberMisscheduled @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
  DaemonSetStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sDaemonsetMisscheduledNodes =
    "k8s.daemonset.misscheduled_nodes";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sDaemonsetMisscheduledNodes =
    "Deprecated, use `k8s.daemonset.node.misscheduled` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sDaemonsetMisscheduledNodes =
    "{node}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetMisscheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetMisscheduledNodes,
                                         descrMetricK8sDaemonsetMisscheduledNodes,
                                         unitMetricK8sDaemonsetMisscheduledNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetMisscheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetMisscheduledNodes,
                                          descrMetricK8sDaemonsetMisscheduledNodes,
                                          unitMetricK8sDaemonsetMisscheduledNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetMisscheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetMisscheduledNodes,
                                                   descrMetricK8sDaemonsetMisscheduledNodes,
                                                   unitMetricK8sDaemonsetMisscheduledNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetMisscheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetMisscheduledNodes,
                                                    descrMetricK8sDaemonsetMisscheduledNodes,
                                                    unitMetricK8sDaemonsetMisscheduledNodes);
}

/**
  Number of nodes that are running at least 1 daemon pod and are supposed to run the daemon pod.
  <p>
  This metric aligns with the @code currentNumberScheduled @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
  DaemonSetStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sDaemonsetNodeCurrentScheduled =
    "k8s.daemonset.node.current_scheduled";
static constexpr const char *descrMetricK8sDaemonsetNodeCurrentScheduled =
    "Number of nodes that are running at least 1 daemon pod and are supposed to run the daemon "
    "pod.";
static constexpr const char *unitMetricK8sDaemonsetNodeCurrentScheduled = "{node}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetNodeCurrentScheduled(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetNodeCurrentScheduled,
                                         descrMetricK8sDaemonsetNodeCurrentScheduled,
                                         unitMetricK8sDaemonsetNodeCurrentScheduled);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetNodeCurrentScheduled(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetNodeCurrentScheduled,
                                          descrMetricK8sDaemonsetNodeCurrentScheduled,
                                          unitMetricK8sDaemonsetNodeCurrentScheduled);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetNodeCurrentScheduled(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetNodeCurrentScheduled,
                                                   descrMetricK8sDaemonsetNodeCurrentScheduled,
                                                   unitMetricK8sDaemonsetNodeCurrentScheduled);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetNodeCurrentScheduled(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetNodeCurrentScheduled,
                                                    descrMetricK8sDaemonsetNodeCurrentScheduled,
                                                    unitMetricK8sDaemonsetNodeCurrentScheduled);
}

/**
  Number of nodes that should be running the daemon pod (including nodes currently running the
  daemon pod). <p> This metric aligns with the @code desiredNumberScheduled @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
  DaemonSetStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sDaemonsetNodeDesiredScheduled =
    "k8s.daemonset.node.desired_scheduled";
static constexpr const char *descrMetricK8sDaemonsetNodeDesiredScheduled =
    "Number of nodes that should be running the daemon pod (including nodes currently running the "
    "daemon pod).";
static constexpr const char *unitMetricK8sDaemonsetNodeDesiredScheduled = "{node}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetNodeDesiredScheduled(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetNodeDesiredScheduled,
                                         descrMetricK8sDaemonsetNodeDesiredScheduled,
                                         unitMetricK8sDaemonsetNodeDesiredScheduled);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetNodeDesiredScheduled(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetNodeDesiredScheduled,
                                          descrMetricK8sDaemonsetNodeDesiredScheduled,
                                          unitMetricK8sDaemonsetNodeDesiredScheduled);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetNodeDesiredScheduled(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetNodeDesiredScheduled,
                                                   descrMetricK8sDaemonsetNodeDesiredScheduled,
                                                   unitMetricK8sDaemonsetNodeDesiredScheduled);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetNodeDesiredScheduled(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetNodeDesiredScheduled,
                                                    descrMetricK8sDaemonsetNodeDesiredScheduled,
                                                    unitMetricK8sDaemonsetNodeDesiredScheduled);
}

/**
  Number of nodes that are running the daemon pod, but are not supposed to run the daemon pod.
  <p>
  This metric aligns with the @code numberMisscheduled @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
  DaemonSetStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sDaemonsetNodeMisscheduled =
    "k8s.daemonset.node.misscheduled";
static constexpr const char *descrMetricK8sDaemonsetNodeMisscheduled =
    "Number of nodes that are running the daemon pod, but are not supposed to run the daemon pod.";
static constexpr const char *unitMetricK8sDaemonsetNodeMisscheduled = "{node}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetNodeMisscheduled(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetNodeMisscheduled,
                                         descrMetricK8sDaemonsetNodeMisscheduled,
                                         unitMetricK8sDaemonsetNodeMisscheduled);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetNodeMisscheduled(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetNodeMisscheduled,
                                          descrMetricK8sDaemonsetNodeMisscheduled,
                                          unitMetricK8sDaemonsetNodeMisscheduled);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetNodeMisscheduled(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetNodeMisscheduled,
                                                   descrMetricK8sDaemonsetNodeMisscheduled,
                                                   unitMetricK8sDaemonsetNodeMisscheduled);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetNodeMisscheduled(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetNodeMisscheduled,
                                                    descrMetricK8sDaemonsetNodeMisscheduled,
                                                    unitMetricK8sDaemonsetNodeMisscheduled);
}

/**
  Number of nodes that should be running the daemon pod and have one or more of the daemon pod
  running and ready. <p> This metric aligns with the @code numberReady @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
  DaemonSetStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sDaemonsetNodeReady = "k8s.daemonset.node.ready";
static constexpr const char *descrMetricK8sDaemonsetNodeReady =
    "Number of nodes that should be running the daemon pod and have one or more of the daemon pod "
    "running and ready.";
static constexpr const char *unitMetricK8sDaemonsetNodeReady = "{node}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetNodeReady(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetNodeReady,
                                         descrMetricK8sDaemonsetNodeReady,
                                         unitMetricK8sDaemonsetNodeReady);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetNodeReady(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetNodeReady,
                                          descrMetricK8sDaemonsetNodeReady,
                                          unitMetricK8sDaemonsetNodeReady);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetNodeReady(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetNodeReady,
                                                   descrMetricK8sDaemonsetNodeReady,
                                                   unitMetricK8sDaemonsetNodeReady);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetNodeReady(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetNodeReady,
                                                    descrMetricK8sDaemonsetNodeReady,
                                                    unitMetricK8sDaemonsetNodeReady);
}

/**
  Deprecated, use @code k8s.daemonset.node.ready @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.daemonset.node.ready @endcode.", "reason": "renamed",
  "renamed_to": "k8s.daemonset.node.ready"} <p> This metric aligns with the @code numberReady
  @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
  DaemonSetStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sDaemonsetReadyNodes =
    "k8s.daemonset.ready_nodes";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sDaemonsetReadyNodes =
    "Deprecated, use `k8s.daemonset.node.ready` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sDaemonsetReadyNodes = "{node}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetReadyNodes(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetReadyNodes,
                                         descrMetricK8sDaemonsetReadyNodes,
                                         unitMetricK8sDaemonsetReadyNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetReadyNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetReadyNodes,
                                          descrMetricK8sDaemonsetReadyNodes,
                                          unitMetricK8sDaemonsetReadyNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetReadyNodes(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetReadyNodes,
                                                   descrMetricK8sDaemonsetReadyNodes,
                                                   unitMetricK8sDaemonsetReadyNodes);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetReadyNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetReadyNodes,
                                                    descrMetricK8sDaemonsetReadyNodes,
                                                    unitMetricK8sDaemonsetReadyNodes);
}

/**
  Deprecated, use @code k8s.deployment.pod.available @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.deployment.pod.available @endcode.", "reason": "renamed",
  "renamed_to": "k8s.deployment.pod.available"} <p> This metric aligns with the @code
  availableReplicas @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#deploymentstatus-v1-apps">K8s
  DeploymentStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sDeploymentAvailablePods =
    "k8s.deployment.available_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sDeploymentAvailablePods =
    "Deprecated, use `k8s.deployment.pod.available` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sDeploymentAvailablePods =
    "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDeploymentAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDeploymentAvailablePods,
                                         descrMetricK8sDeploymentAvailablePods,
                                         unitMetricK8sDeploymentAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDeploymentAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDeploymentAvailablePods,
                                          descrMetricK8sDeploymentAvailablePods,
                                          unitMetricK8sDeploymentAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDeploymentAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDeploymentAvailablePods,
                                                   descrMetricK8sDeploymentAvailablePods,
                                                   unitMetricK8sDeploymentAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDeploymentAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDeploymentAvailablePods,
                                                    descrMetricK8sDeploymentAvailablePods,
                                                    unitMetricK8sDeploymentAvailablePods);
}

/**
  Deprecated, use @code k8s.deployment.pod.desired @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.deployment.pod.desired @endcode.", "reason": "renamed",
  "renamed_to": "k8s.deployment.pod.desired"} <p> This metric aligns with the @code replicas
  @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#deploymentspec-v1-apps">K8s
  DeploymentSpec</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sDeploymentDesiredPods =
    "k8s.deployment.desired_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sDeploymentDesiredPods =
    "Deprecated, use `k8s.deployment.pod.desired` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sDeploymentDesiredPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDeploymentDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDeploymentDesiredPods,
                                         descrMetricK8sDeploymentDesiredPods,
                                         unitMetricK8sDeploymentDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDeploymentDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDeploymentDesiredPods,
                                          descrMetricK8sDeploymentDesiredPods,
                                          unitMetricK8sDeploymentDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDeploymentDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDeploymentDesiredPods,
                                                   descrMetricK8sDeploymentDesiredPods,
                                                   unitMetricK8sDeploymentDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDeploymentDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDeploymentDesiredPods,
                                                    descrMetricK8sDeploymentDesiredPods,
                                                    unitMetricK8sDeploymentDesiredPods);
}

/**
  Total number of available replica pods (ready for at least minReadySeconds) targeted by this
  deployment. <p> This metric aligns with the @code availableReplicas @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#deploymentstatus-v1-apps">K8s
  DeploymentStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sDeploymentPodAvailable = "k8s.deployment.pod.available";
static constexpr const char *descrMetricK8sDeploymentPodAvailable =
    "Total number of available replica pods (ready for at least minReadySeconds) targeted by this "
    "deployment.";
static constexpr const char *unitMetricK8sDeploymentPodAvailable = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDeploymentPodAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDeploymentPodAvailable,
                                         descrMetricK8sDeploymentPodAvailable,
                                         unitMetricK8sDeploymentPodAvailable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDeploymentPodAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDeploymentPodAvailable,
                                          descrMetricK8sDeploymentPodAvailable,
                                          unitMetricK8sDeploymentPodAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDeploymentPodAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDeploymentPodAvailable,
                                                   descrMetricK8sDeploymentPodAvailable,
                                                   unitMetricK8sDeploymentPodAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDeploymentPodAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDeploymentPodAvailable,
                                                    descrMetricK8sDeploymentPodAvailable,
                                                    unitMetricK8sDeploymentPodAvailable);
}

/**
  Number of desired replica pods in this deployment.
  <p>
  This metric aligns with the @code replicas @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#deploymentspec-v1-apps">K8s
  DeploymentSpec</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sDeploymentPodDesired = "k8s.deployment.pod.desired";
static constexpr const char *descrMetricK8sDeploymentPodDesired =
    "Number of desired replica pods in this deployment.";
static constexpr const char *unitMetricK8sDeploymentPodDesired = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDeploymentPodDesired(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDeploymentPodDesired,
                                         descrMetricK8sDeploymentPodDesired,
                                         unitMetricK8sDeploymentPodDesired);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDeploymentPodDesired(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDeploymentPodDesired,
                                          descrMetricK8sDeploymentPodDesired,
                                          unitMetricK8sDeploymentPodDesired);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDeploymentPodDesired(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDeploymentPodDesired,
                                                   descrMetricK8sDeploymentPodDesired,
                                                   unitMetricK8sDeploymentPodDesired);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDeploymentPodDesired(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDeploymentPodDesired,
                                                    descrMetricK8sDeploymentPodDesired,
                                                    unitMetricK8sDeploymentPodDesired);
}

/**
  Deprecated, use @code k8s.hpa.pod.current @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.hpa.pod.current @endcode.", "reason": "renamed", "renamed_to":
  "k8s.hpa.pod.current"} <p> This metric aligns with the @code currentReplicas @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerstatus-v2-autoscaling">K8s
  HorizontalPodAutoscalerStatus</a> <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sHpaCurrentPods =
    "k8s.hpa.current_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sHpaCurrentPods =
    "Deprecated, use `k8s.hpa.pod.current` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sHpaCurrentPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sHpaCurrentPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaCurrentPods, descrMetricK8sHpaCurrentPods,
                                         unitMetricK8sHpaCurrentPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sHpaCurrentPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaCurrentPods, descrMetricK8sHpaCurrentPods,
                                          unitMetricK8sHpaCurrentPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaCurrentPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sHpaCurrentPods, descrMetricK8sHpaCurrentPods, unitMetricK8sHpaCurrentPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaCurrentPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sHpaCurrentPods, descrMetricK8sHpaCurrentPods, unitMetricK8sHpaCurrentPods);
}

/**
  Deprecated, use @code k8s.hpa.pod.desired @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.hpa.pod.desired @endcode.", "reason": "renamed", "renamed_to":
  "k8s.hpa.pod.desired"} <p> This metric aligns with the @code desiredReplicas @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerstatus-v2-autoscaling">K8s
  HorizontalPodAutoscalerStatus</a> <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sHpaDesiredPods =
    "k8s.hpa.desired_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sHpaDesiredPods =
    "Deprecated, use `k8s.hpa.pod.desired` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sHpaDesiredPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sHpaDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaDesiredPods, descrMetricK8sHpaDesiredPods,
                                         unitMetricK8sHpaDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sHpaDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaDesiredPods, descrMetricK8sHpaDesiredPods,
                                          unitMetricK8sHpaDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sHpaDesiredPods, descrMetricK8sHpaDesiredPods, unitMetricK8sHpaDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sHpaDesiredPods, descrMetricK8sHpaDesiredPods, unitMetricK8sHpaDesiredPods);
}

/**
  Deprecated, use @code k8s.hpa.pod.max @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.hpa.pod.max @endcode.", "reason": "renamed", "renamed_to":
  "k8s.hpa.pod.max"} <p> This metric aligns with the @code maxReplicas @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerspec-v2-autoscaling">K8s
  HorizontalPodAutoscalerSpec</a> <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sHpaMaxPods = "k8s.hpa.max_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sHpaMaxPods =
    "Deprecated, use `k8s.hpa.pod.max` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sHpaMaxPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sHpaMaxPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaMaxPods, descrMetricK8sHpaMaxPods,
                                         unitMetricK8sHpaMaxPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sHpaMaxPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaMaxPods, descrMetricK8sHpaMaxPods,
                                          unitMetricK8sHpaMaxPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaMaxPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sHpaMaxPods, descrMetricK8sHpaMaxPods,
                                                   unitMetricK8sHpaMaxPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaMaxPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sHpaMaxPods, descrMetricK8sHpaMaxPods,
                                                    unitMetricK8sHpaMaxPods);
}

/**
  Target average utilization, in percentage, for CPU resource in HPA config.
  <p>
  This metric aligns with the @code averageUtilization @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#metrictarget-v2-autoscaling">K8s
  HPA MetricTarget</a>. If the type of the metric is <a
  href="https://kubernetes.io/docs/tasks/run-application/horizontal-pod-autoscale/#support-for-metrics-apis">@code
  ContainerResource @endcode</a>, the @code k8s.container.name @endcode attribute MUST be set to
  identify the specific container within the pod to which the metric applies. <p> gauge
 */
static constexpr const char *kMetricK8sHpaMetricTargetCpuAverageUtilization =
    "k8s.hpa.metric.target.cpu.average_utilization";
static constexpr const char *descrMetricK8sHpaMetricTargetCpuAverageUtilization =
    "Target average utilization, in percentage, for CPU resource in HPA config.";
static constexpr const char *unitMetricK8sHpaMetricTargetCpuAverageUtilization = "1";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricK8sHpaMetricTargetCpuAverageUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sHpaMetricTargetCpuAverageUtilization,
                                 descrMetricK8sHpaMetricTargetCpuAverageUtilization,
                                 unitMetricK8sHpaMetricTargetCpuAverageUtilization);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricK8sHpaMetricTargetCpuAverageUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sHpaMetricTargetCpuAverageUtilization,
                                  descrMetricK8sHpaMetricTargetCpuAverageUtilization,
                                  unitMetricK8sHpaMetricTargetCpuAverageUtilization);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaMetricTargetCpuAverageUtilization(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sHpaMetricTargetCpuAverageUtilization,
                                           descrMetricK8sHpaMetricTargetCpuAverageUtilization,
                                           unitMetricK8sHpaMetricTargetCpuAverageUtilization);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaMetricTargetCpuAverageUtilization(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricK8sHpaMetricTargetCpuAverageUtilization,
                                            descrMetricK8sHpaMetricTargetCpuAverageUtilization,
                                            unitMetricK8sHpaMetricTargetCpuAverageUtilization);
}

/**
  Target average value for CPU resource in HPA config.
  <p>
  This metric aligns with the @code averageValue @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#metrictarget-v2-autoscaling">K8s
  HPA MetricTarget</a>. If the type of the metric is <a
  href="https://kubernetes.io/docs/tasks/run-application/horizontal-pod-autoscale/#support-for-metrics-apis">@code
  ContainerResource @endcode</a>, the @code k8s.container.name @endcode attribute MUST be set to
  identify the specific container within the pod to which the metric applies. <p> gauge
 */
static constexpr const char *kMetricK8sHpaMetricTargetCpuAverageValue =
    "k8s.hpa.metric.target.cpu.average_value";
static constexpr const char *descrMetricK8sHpaMetricTargetCpuAverageValue =
    "Target average value for CPU resource in HPA config.";
static constexpr const char *unitMetricK8sHpaMetricTargetCpuAverageValue = "{cpu}";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricK8sHpaMetricTargetCpuAverageValue(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sHpaMetricTargetCpuAverageValue,
                                 descrMetricK8sHpaMetricTargetCpuAverageValue,
                                 unitMetricK8sHpaMetricTargetCpuAverageValue);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricK8sHpaMetricTargetCpuAverageValue(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sHpaMetricTargetCpuAverageValue,
                                  descrMetricK8sHpaMetricTargetCpuAverageValue,
                                  unitMetricK8sHpaMetricTargetCpuAverageValue);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaMetricTargetCpuAverageValue(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sHpaMetricTargetCpuAverageValue,
                                           descrMetricK8sHpaMetricTargetCpuAverageValue,
                                           unitMetricK8sHpaMetricTargetCpuAverageValue);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaMetricTargetCpuAverageValue(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricK8sHpaMetricTargetCpuAverageValue,
                                            descrMetricK8sHpaMetricTargetCpuAverageValue,
                                            unitMetricK8sHpaMetricTargetCpuAverageValue);
}

/**
  Target value for CPU resource in HPA config.
  <p>
  This metric aligns with the @code value @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#metrictarget-v2-autoscaling">K8s
  HPA MetricTarget</a>. If the type of the metric is <a
  href="https://kubernetes.io/docs/tasks/run-application/horizontal-pod-autoscale/#support-for-metrics-apis">@code
  ContainerResource @endcode</a>, the @code k8s.container.name @endcode attribute MUST be set to
  identify the specific container within the pod to which the metric applies. <p> gauge
 */
static constexpr const char *kMetricK8sHpaMetricTargetCpuValue = "k8s.hpa.metric.target.cpu.value";
static constexpr const char *descrMetricK8sHpaMetricTargetCpuValue =
    "Target value for CPU resource in HPA config.";
static constexpr const char *unitMetricK8sHpaMetricTargetCpuValue = "{cpu}";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>>
CreateSyncInt64MetricK8sHpaMetricTargetCpuValue(metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sHpaMetricTargetCpuValue,
                                 descrMetricK8sHpaMetricTargetCpuValue,
                                 unitMetricK8sHpaMetricTargetCpuValue);
}

static inline nostd::unique_ptr<metrics::Gauge<double>>
CreateSyncDoubleMetricK8sHpaMetricTargetCpuValue(metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sHpaMetricTargetCpuValue,
                                  descrMetricK8sHpaMetricTargetCpuValue,
                                  unitMetricK8sHpaMetricTargetCpuValue);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaMetricTargetCpuValue(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sHpaMetricTargetCpuValue,
                                           descrMetricK8sHpaMetricTargetCpuValue,
                                           unitMetricK8sHpaMetricTargetCpuValue);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaMetricTargetCpuValue(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricK8sHpaMetricTargetCpuValue,
                                            descrMetricK8sHpaMetricTargetCpuValue,
                                            unitMetricK8sHpaMetricTargetCpuValue);
}

/**
  Deprecated, use @code k8s.hpa.pod.min @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.hpa.pod.min @endcode.", "reason": "renamed", "renamed_to":
  "k8s.hpa.pod.min"} <p> This metric aligns with the @code minReplicas @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerspec-v2-autoscaling">K8s
  HorizontalPodAutoscalerSpec</a> <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sHpaMinPods = "k8s.hpa.min_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sHpaMinPods =
    "Deprecated, use `k8s.hpa.pod.min` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sHpaMinPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sHpaMinPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaMinPods, descrMetricK8sHpaMinPods,
                                         unitMetricK8sHpaMinPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sHpaMinPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaMinPods, descrMetricK8sHpaMinPods,
                                          unitMetricK8sHpaMinPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaMinPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sHpaMinPods, descrMetricK8sHpaMinPods,
                                                   unitMetricK8sHpaMinPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaMinPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sHpaMinPods, descrMetricK8sHpaMinPods,
                                                    unitMetricK8sHpaMinPods);
}

/**
  Current number of replica pods managed by this horizontal pod autoscaler, as last seen by the
  autoscaler. <p> This metric aligns with the @code currentReplicas @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerstatus-v2-autoscaling">K8s
  HorizontalPodAutoscalerStatus</a> <p> updowncounter
 */
static constexpr const char *kMetricK8sHpaPodCurrent = "k8s.hpa.pod.current";
static constexpr const char *descrMetricK8sHpaPodCurrent =
    "Current number of replica pods managed by this horizontal pod autoscaler, as last seen by the "
    "autoscaler.";
static constexpr const char *unitMetricK8sHpaPodCurrent = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sHpaPodCurrent(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaPodCurrent, descrMetricK8sHpaPodCurrent,
                                         unitMetricK8sHpaPodCurrent);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sHpaPodCurrent(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaPodCurrent, descrMetricK8sHpaPodCurrent,
                                          unitMetricK8sHpaPodCurrent);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaPodCurrent(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sHpaPodCurrent, descrMetricK8sHpaPodCurrent, unitMetricK8sHpaPodCurrent);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaPodCurrent(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sHpaPodCurrent, descrMetricK8sHpaPodCurrent, unitMetricK8sHpaPodCurrent);
}

/**
  Desired number of replica pods managed by this horizontal pod autoscaler, as last calculated by
  the autoscaler. <p> This metric aligns with the @code desiredReplicas @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerstatus-v2-autoscaling">K8s
  HorizontalPodAutoscalerStatus</a> <p> updowncounter
 */
static constexpr const char *kMetricK8sHpaPodDesired = "k8s.hpa.pod.desired";
static constexpr const char *descrMetricK8sHpaPodDesired =
    "Desired number of replica pods managed by this horizontal pod autoscaler, as last calculated "
    "by the autoscaler.";
static constexpr const char *unitMetricK8sHpaPodDesired = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sHpaPodDesired(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaPodDesired, descrMetricK8sHpaPodDesired,
                                         unitMetricK8sHpaPodDesired);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sHpaPodDesired(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaPodDesired, descrMetricK8sHpaPodDesired,
                                          unitMetricK8sHpaPodDesired);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaPodDesired(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sHpaPodDesired, descrMetricK8sHpaPodDesired, unitMetricK8sHpaPodDesired);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaPodDesired(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sHpaPodDesired, descrMetricK8sHpaPodDesired, unitMetricK8sHpaPodDesired);
}

/**
  The upper limit for the number of replica pods to which the autoscaler can scale up.
  <p>
  This metric aligns with the @code maxReplicas @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerspec-v2-autoscaling">K8s
  HorizontalPodAutoscalerSpec</a> <p> updowncounter
 */
static constexpr const char *kMetricK8sHpaPodMax = "k8s.hpa.pod.max";
static constexpr const char *descrMetricK8sHpaPodMax =
    "The upper limit for the number of replica pods to which the autoscaler can scale up.";
static constexpr const char *unitMetricK8sHpaPodMax = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>> CreateSyncInt64MetricK8sHpaPodMax(
    metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaPodMax, descrMetricK8sHpaPodMax,
                                         unitMetricK8sHpaPodMax);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>> CreateSyncDoubleMetricK8sHpaPodMax(
    metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaPodMax, descrMetricK8sHpaPodMax,
                                          unitMetricK8sHpaPodMax);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricK8sHpaPodMax(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sHpaPodMax, descrMetricK8sHpaPodMax,
                                                   unitMetricK8sHpaPodMax);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricK8sHpaPodMax(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sHpaPodMax, descrMetricK8sHpaPodMax,
                                                    unitMetricK8sHpaPodMax);
}

/**
  The lower limit for the number of replica pods to which the autoscaler can scale down.
  <p>
  This metric aligns with the @code minReplicas @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerspec-v2-autoscaling">K8s
  HorizontalPodAutoscalerSpec</a> <p> updowncounter
 */
static constexpr const char *kMetricK8sHpaPodMin = "k8s.hpa.pod.min";
static constexpr const char *descrMetricK8sHpaPodMin =
    "The lower limit for the number of replica pods to which the autoscaler can scale down.";
static constexpr const char *unitMetricK8sHpaPodMin = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>> CreateSyncInt64MetricK8sHpaPodMin(
    metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaPodMin, descrMetricK8sHpaPodMin,
                                         unitMetricK8sHpaPodMin);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>> CreateSyncDoubleMetricK8sHpaPodMin(
    metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaPodMin, descrMetricK8sHpaPodMin,
                                          unitMetricK8sHpaPodMin);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricK8sHpaPodMin(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sHpaPodMin, descrMetricK8sHpaPodMin,
                                                   unitMetricK8sHpaPodMin);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricK8sHpaPodMin(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sHpaPodMin, descrMetricK8sHpaPodMin,
                                                    unitMetricK8sHpaPodMin);
}

/**
  Deprecated, use @code k8s.job.pod.active @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.job.pod.active @endcode.", "reason": "renamed", "renamed_to":
  "k8s.job.pod.active"} <p> This metric aligns with the @code active @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobstatus-v1-batch">K8s
  JobStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sJobActivePods =
    "k8s.job.active_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sJobActivePods =
    "Deprecated, use `k8s.job.pod.active` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sJobActivePods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobActivePods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobActivePods, descrMetricK8sJobActivePods,
                                         unitMetricK8sJobActivePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobActivePods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobActivePods, descrMetricK8sJobActivePods,
                                          unitMetricK8sJobActivePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobActivePods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sJobActivePods, descrMetricK8sJobActivePods, unitMetricK8sJobActivePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobActivePods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sJobActivePods, descrMetricK8sJobActivePods, unitMetricK8sJobActivePods);
}

/**
  Deprecated, use @code k8s.job.pod.desired_successful @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.job.pod.desired_successful @endcode.", "reason": "renamed",
  "renamed_to": "k8s.job.pod.desired_successful"} <p> This metric aligns with the @code completions
  @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobspec-v1-batch">K8s
  JobSpec</a>.. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sJobDesiredSuccessfulPods =
    "k8s.job.desired_successful_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sJobDesiredSuccessfulPods =
    "Deprecated, use `k8s.job.pod.desired_successful` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sJobDesiredSuccessfulPods =
    "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobDesiredSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobDesiredSuccessfulPods,
                                         descrMetricK8sJobDesiredSuccessfulPods,
                                         unitMetricK8sJobDesiredSuccessfulPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobDesiredSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobDesiredSuccessfulPods,
                                          descrMetricK8sJobDesiredSuccessfulPods,
                                          unitMetricK8sJobDesiredSuccessfulPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobDesiredSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sJobDesiredSuccessfulPods,
                                                   descrMetricK8sJobDesiredSuccessfulPods,
                                                   unitMetricK8sJobDesiredSuccessfulPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobDesiredSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sJobDesiredSuccessfulPods,
                                                    descrMetricK8sJobDesiredSuccessfulPods,
                                                    unitMetricK8sJobDesiredSuccessfulPods);
}

/**
  Deprecated, use @code k8s.job.pod.failed @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.job.pod.failed @endcode.", "reason": "renamed", "renamed_to":
  "k8s.job.pod.failed"} <p> This metric aligns with the @code failed @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobstatus-v1-batch">K8s
  JobStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sJobFailedPods =
    "k8s.job.failed_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sJobFailedPods =
    "Deprecated, use `k8s.job.pod.failed` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sJobFailedPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobFailedPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobFailedPods, descrMetricK8sJobFailedPods,
                                         unitMetricK8sJobFailedPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobFailedPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobFailedPods, descrMetricK8sJobFailedPods,
                                          unitMetricK8sJobFailedPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobFailedPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sJobFailedPods, descrMetricK8sJobFailedPods, unitMetricK8sJobFailedPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobFailedPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sJobFailedPods, descrMetricK8sJobFailedPods, unitMetricK8sJobFailedPods);
}

/**
  Deprecated, use @code k8s.job.pod.max_parallel @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.job.pod.max_parallel @endcode.", "reason": "renamed",
  "renamed_to": "k8s.job.pod.max_parallel"} <p> This metric aligns with the @code parallelism
  @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobspec-v1-batch">K8s
  JobSpec</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sJobMaxParallelPods =
    "k8s.job.max_parallel_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sJobMaxParallelPods =
    "Deprecated, use `k8s.job.pod.max_parallel` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sJobMaxParallelPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobMaxParallelPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobMaxParallelPods,
                                         descrMetricK8sJobMaxParallelPods,
                                         unitMetricK8sJobMaxParallelPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobMaxParallelPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobMaxParallelPods,
                                          descrMetricK8sJobMaxParallelPods,
                                          unitMetricK8sJobMaxParallelPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobMaxParallelPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sJobMaxParallelPods,
                                                   descrMetricK8sJobMaxParallelPods,
                                                   unitMetricK8sJobMaxParallelPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobMaxParallelPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sJobMaxParallelPods,
                                                    descrMetricK8sJobMaxParallelPods,
                                                    unitMetricK8sJobMaxParallelPods);
}

/**
  The number of pending and actively running pods for a job.
  <p>
  This metric aligns with the @code active @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobstatus-v1-batch">K8s
  JobStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sJobPodActive = "k8s.job.pod.active";
static constexpr const char *descrMetricK8sJobPodActive =
    "The number of pending and actively running pods for a job.";
static constexpr const char *unitMetricK8sJobPodActive = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobPodActive(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobPodActive, descrMetricK8sJobPodActive,
                                         unitMetricK8sJobPodActive);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobPodActive(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobPodActive, descrMetricK8sJobPodActive,
                                          unitMetricK8sJobPodActive);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobPodActive(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sJobPodActive, descrMetricK8sJobPodActive, unitMetricK8sJobPodActive);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobPodActive(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sJobPodActive, descrMetricK8sJobPodActive, unitMetricK8sJobPodActive);
}

/**
  The desired number of successfully finished pods the job should be run with.
  <p>
  This metric aligns with the @code completions @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobspec-v1-batch">K8s
  JobSpec</a>.. <p> updowncounter
 */
static constexpr const char *kMetricK8sJobPodDesiredSuccessful = "k8s.job.pod.desired_successful";
static constexpr const char *descrMetricK8sJobPodDesiredSuccessful =
    "The desired number of successfully finished pods the job should be run with.";
static constexpr const char *unitMetricK8sJobPodDesiredSuccessful = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobPodDesiredSuccessful(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobPodDesiredSuccessful,
                                         descrMetricK8sJobPodDesiredSuccessful,
                                         unitMetricK8sJobPodDesiredSuccessful);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobPodDesiredSuccessful(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobPodDesiredSuccessful,
                                          descrMetricK8sJobPodDesiredSuccessful,
                                          unitMetricK8sJobPodDesiredSuccessful);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobPodDesiredSuccessful(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sJobPodDesiredSuccessful,
                                                   descrMetricK8sJobPodDesiredSuccessful,
                                                   unitMetricK8sJobPodDesiredSuccessful);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobPodDesiredSuccessful(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sJobPodDesiredSuccessful,
                                                    descrMetricK8sJobPodDesiredSuccessful,
                                                    unitMetricK8sJobPodDesiredSuccessful);
}

/**
  The number of pods which reached phase Failed for a job.
  <p>
  This metric aligns with the @code failed @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobstatus-v1-batch">K8s
  JobStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sJobPodFailed = "k8s.job.pod.failed";
static constexpr const char *descrMetricK8sJobPodFailed =
    "The number of pods which reached phase Failed for a job.";
static constexpr const char *unitMetricK8sJobPodFailed = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobPodFailed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobPodFailed, descrMetricK8sJobPodFailed,
                                         unitMetricK8sJobPodFailed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobPodFailed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobPodFailed, descrMetricK8sJobPodFailed,
                                          unitMetricK8sJobPodFailed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobPodFailed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sJobPodFailed, descrMetricK8sJobPodFailed, unitMetricK8sJobPodFailed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobPodFailed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sJobPodFailed, descrMetricK8sJobPodFailed, unitMetricK8sJobPodFailed);
}

/**
  The max desired number of pods the job should run at any given time.
  <p>
  This metric aligns with the @code parallelism @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobspec-v1-batch">K8s
  JobSpec</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sJobPodMaxParallel = "k8s.job.pod.max_parallel";
static constexpr const char *descrMetricK8sJobPodMaxParallel =
    "The max desired number of pods the job should run at any given time.";
static constexpr const char *unitMetricK8sJobPodMaxParallel = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobPodMaxParallel(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricK8sJobPodMaxParallel, descrMetricK8sJobPodMaxParallel, unitMetricK8sJobPodMaxParallel);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobPodMaxParallel(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricK8sJobPodMaxParallel, descrMetricK8sJobPodMaxParallel, unitMetricK8sJobPodMaxParallel);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobPodMaxParallel(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sJobPodMaxParallel, descrMetricK8sJobPodMaxParallel, unitMetricK8sJobPodMaxParallel);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobPodMaxParallel(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sJobPodMaxParallel, descrMetricK8sJobPodMaxParallel, unitMetricK8sJobPodMaxParallel);
}

/**
  The number of pods which reached phase Succeeded for a job.
  <p>
  This metric aligns with the @code succeeded @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobstatus-v1-batch">K8s
  JobStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sJobPodSuccessful = "k8s.job.pod.successful";
static constexpr const char *descrMetricK8sJobPodSuccessful =
    "The number of pods which reached phase Succeeded for a job.";
static constexpr const char *unitMetricK8sJobPodSuccessful = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobPodSuccessful(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobPodSuccessful, descrMetricK8sJobPodSuccessful,
                                         unitMetricK8sJobPodSuccessful);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobPodSuccessful(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricK8sJobPodSuccessful, descrMetricK8sJobPodSuccessful, unitMetricK8sJobPodSuccessful);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobPodSuccessful(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sJobPodSuccessful, descrMetricK8sJobPodSuccessful, unitMetricK8sJobPodSuccessful);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobPodSuccessful(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sJobPodSuccessful, descrMetricK8sJobPodSuccessful, unitMetricK8sJobPodSuccessful);
}

/**
  Deprecated, use @code k8s.job.pod.successful @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.job.pod.successful @endcode.", "reason": "renamed", "renamed_to":
  "k8s.job.pod.successful"} <p> This metric aligns with the @code succeeded @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobstatus-v1-batch">K8s
  JobStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sJobSuccessfulPods =
    "k8s.job.successful_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sJobSuccessfulPods =
    "Deprecated, use `k8s.job.pod.successful` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sJobSuccessfulPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricK8sJobSuccessfulPods, descrMetricK8sJobSuccessfulPods, unitMetricK8sJobSuccessfulPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricK8sJobSuccessfulPods, descrMetricK8sJobSuccessfulPods, unitMetricK8sJobSuccessfulPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sJobSuccessfulPods, descrMetricK8sJobSuccessfulPods, unitMetricK8sJobSuccessfulPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sJobSuccessfulPods, descrMetricK8sJobSuccessfulPods, unitMetricK8sJobSuccessfulPods);
}

/**
  Describes number of K8s namespaces that are currently in a given phase.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sNamespacePhase = "k8s.namespace.phase";
static constexpr const char *descrMetricK8sNamespacePhase =
    "Describes number of K8s namespaces that are currently in a given phase.";
static constexpr const char *unitMetricK8sNamespacePhase = "{namespace}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNamespacePhase(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNamespacePhase, descrMetricK8sNamespacePhase,
                                         unitMetricK8sNamespacePhase);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNamespacePhase(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNamespacePhase, descrMetricK8sNamespacePhase,
                                          unitMetricK8sNamespacePhase);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNamespacePhase(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sNamespacePhase, descrMetricK8sNamespacePhase, unitMetricK8sNamespacePhase);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNamespacePhase(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sNamespacePhase, descrMetricK8sNamespacePhase, unitMetricK8sNamespacePhase);
}

/**
  Deprecated, use @code k8s.node.cpu.allocatable @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.node.cpu.allocatable @endcode.", "reason": "renamed",
  "renamed_to": "k8s.node.cpu.allocatable"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sNodeAllocatableCpu =
    "k8s.node.allocatable.cpu";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sNodeAllocatableCpu =
    "Deprecated, use `k8s.node.cpu.allocatable` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sNodeAllocatableCpu = "{cpu}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeAllocatableCpu(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeAllocatableCpu,
                                         descrMetricK8sNodeAllocatableCpu,
                                         unitMetricK8sNodeAllocatableCpu);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeAllocatableCpu(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeAllocatableCpu,
                                          descrMetricK8sNodeAllocatableCpu,
                                          unitMetricK8sNodeAllocatableCpu);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeAllocatableCpu(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeAllocatableCpu,
                                                   descrMetricK8sNodeAllocatableCpu,
                                                   unitMetricK8sNodeAllocatableCpu);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeAllocatableCpu(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeAllocatableCpu,
                                                    descrMetricK8sNodeAllocatableCpu,
                                                    unitMetricK8sNodeAllocatableCpu);
}

/**
  Deprecated, use @code k8s.node.ephemeral_storage.allocatable @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.node.ephemeral_storage.allocatable @endcode.", "reason":
  "renamed", "renamed_to": "k8s.node.ephemeral_storage.allocatable"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sNodeAllocatableEphemeralStorage =
    "k8s.node.allocatable.ephemeral_storage";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricK8sNodeAllocatableEphemeralStorage =
        "Deprecated, use `k8s.node.ephemeral_storage.allocatable` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sNodeAllocatableEphemeralStorage =
    "By";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeAllocatableEphemeralStorage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeAllocatableEphemeralStorage,
                                         descrMetricK8sNodeAllocatableEphemeralStorage,
                                         unitMetricK8sNodeAllocatableEphemeralStorage);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeAllocatableEphemeralStorage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeAllocatableEphemeralStorage,
                                          descrMetricK8sNodeAllocatableEphemeralStorage,
                                          unitMetricK8sNodeAllocatableEphemeralStorage);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeAllocatableEphemeralStorage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeAllocatableEphemeralStorage,
                                                   descrMetricK8sNodeAllocatableEphemeralStorage,
                                                   unitMetricK8sNodeAllocatableEphemeralStorage);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeAllocatableEphemeralStorage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeAllocatableEphemeralStorage,
                                                    descrMetricK8sNodeAllocatableEphemeralStorage,
                                                    unitMetricK8sNodeAllocatableEphemeralStorage);
}

/**
  Deprecated, use @code k8s.node.memory.allocatable @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.node.memory.allocatable @endcode.", "reason": "renamed",
  "renamed_to": "k8s.node.memory.allocatable"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sNodeAllocatableMemory =
    "k8s.node.allocatable.memory";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sNodeAllocatableMemory =
    "Deprecated, use `k8s.node.memory.allocatable` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sNodeAllocatableMemory = "By";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeAllocatableMemory(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeAllocatableMemory,
                                         descrMetricK8sNodeAllocatableMemory,
                                         unitMetricK8sNodeAllocatableMemory);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeAllocatableMemory(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeAllocatableMemory,
                                          descrMetricK8sNodeAllocatableMemory,
                                          unitMetricK8sNodeAllocatableMemory);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeAllocatableMemory(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeAllocatableMemory,
                                                   descrMetricK8sNodeAllocatableMemory,
                                                   unitMetricK8sNodeAllocatableMemory);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeAllocatableMemory(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeAllocatableMemory,
                                                    descrMetricK8sNodeAllocatableMemory,
                                                    unitMetricK8sNodeAllocatableMemory);
}

/**
  Deprecated, use @code k8s.node.pod.allocatable @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.node.pod.allocatable @endcode.", "reason": "renamed",
  "renamed_to": "k8s.node.pod.allocatable"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sNodeAllocatablePods =
    "k8s.node.allocatable.pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sNodeAllocatablePods =
    "Deprecated, use `k8s.node.pod.allocatable` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sNodeAllocatablePods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeAllocatablePods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeAllocatablePods,
                                         descrMetricK8sNodeAllocatablePods,
                                         unitMetricK8sNodeAllocatablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeAllocatablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeAllocatablePods,
                                          descrMetricK8sNodeAllocatablePods,
                                          unitMetricK8sNodeAllocatablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeAllocatablePods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeAllocatablePods,
                                                   descrMetricK8sNodeAllocatablePods,
                                                   unitMetricK8sNodeAllocatablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeAllocatablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeAllocatablePods,
                                                    descrMetricK8sNodeAllocatablePods,
                                                    unitMetricK8sNodeAllocatablePods);
}

/**
  Describes the condition of a particular Node.
  <p>
  All possible node condition pairs (type and status) will be reported at each time interval to
  avoid missing metrics. Condition pairs corresponding to the current conditions' statuses will be
  non-zero. <p> updowncounter
 */
static constexpr const char *kMetricK8sNodeConditionStatus = "k8s.node.condition.status";
static constexpr const char *descrMetricK8sNodeConditionStatus =
    "Describes the condition of a particular Node.";
static constexpr const char *unitMetricK8sNodeConditionStatus = "{node}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeConditionStatus(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeConditionStatus,
                                         descrMetricK8sNodeConditionStatus,
                                         unitMetricK8sNodeConditionStatus);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeConditionStatus(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeConditionStatus,
                                          descrMetricK8sNodeConditionStatus,
                                          unitMetricK8sNodeConditionStatus);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeConditionStatus(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeConditionStatus,
                                                   descrMetricK8sNodeConditionStatus,
                                                   unitMetricK8sNodeConditionStatus);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeConditionStatus(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeConditionStatus,
                                                    descrMetricK8sNodeConditionStatus,
                                                    unitMetricK8sNodeConditionStatus);
}

/**
  Amount of cpu allocatable on the node.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sNodeCpuAllocatable = "k8s.node.cpu.allocatable";
static constexpr const char *descrMetricK8sNodeCpuAllocatable =
    "Amount of cpu allocatable on the node.";
static constexpr const char *unitMetricK8sNodeCpuAllocatable = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeCpuAllocatable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeCpuAllocatable,
                                         descrMetricK8sNodeCpuAllocatable,
                                         unitMetricK8sNodeCpuAllocatable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeCpuAllocatable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeCpuAllocatable,
                                          descrMetricK8sNodeCpuAllocatable,
                                          unitMetricK8sNodeCpuAllocatable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeCpuAllocatable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeCpuAllocatable,
                                                   descrMetricK8sNodeCpuAllocatable,
                                                   unitMetricK8sNodeCpuAllocatable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeCpuAllocatable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeCpuAllocatable,
                                                    descrMetricK8sNodeCpuAllocatable,
                                                    unitMetricK8sNodeCpuAllocatable);
}

/**
  Total CPU time consumed.
  <p>
  Total CPU time consumed by the specific Node on all available CPU cores
  <p>
  counter
 */
static constexpr const char *kMetricK8sNodeCpuTime     = "k8s.node.cpu.time";
static constexpr const char *descrMetricK8sNodeCpuTime = "Total CPU time consumed.";
static constexpr const char *unitMetricK8sNodeCpuTime  = "s";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricK8sNodeCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricK8sNodeCpuTime, descrMetricK8sNodeCpuTime,
                                    unitMetricK8sNodeCpuTime);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricK8sNodeCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricK8sNodeCpuTime, descrMetricK8sNodeCpuTime,
                                    unitMetricK8sNodeCpuTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricK8sNodeCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricK8sNodeCpuTime, descrMetricK8sNodeCpuTime,
                                             unitMetricK8sNodeCpuTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeCpuTime(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricK8sNodeCpuTime, descrMetricK8sNodeCpuTime,
                                              unitMetricK8sNodeCpuTime);
}

/**
  Node's CPU usage, measured in cpus. Range from 0 to the number of allocatable CPUs.
  <p>
  CPU usage of the specific Node on all available CPU cores, averaged over the sample window
  <p>
  gauge
 */
static constexpr const char *kMetricK8sNodeCpuUsage = "k8s.node.cpu.usage";
static constexpr const char *descrMetricK8sNodeCpuUsage =
    "Node's CPU usage, measured in cpus. Range from 0 to the number of allocatable CPUs.";
static constexpr const char *unitMetricK8sNodeCpuUsage = "{cpu}";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricK8sNodeCpuUsage(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sNodeCpuUsage, descrMetricK8sNodeCpuUsage,
                                 unitMetricK8sNodeCpuUsage);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricK8sNodeCpuUsage(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sNodeCpuUsage, descrMetricK8sNodeCpuUsage,
                                  unitMetricK8sNodeCpuUsage);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeCpuUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sNodeCpuUsage, descrMetricK8sNodeCpuUsage,
                                           unitMetricK8sNodeCpuUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeCpuUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricK8sNodeCpuUsage, descrMetricK8sNodeCpuUsage,
                                            unitMetricK8sNodeCpuUsage);
}

/**
  Amount of ephemeral-storage allocatable on the node.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sNodeEphemeralStorageAllocatable =
    "k8s.node.ephemeral_storage.allocatable";
static constexpr const char *descrMetricK8sNodeEphemeralStorageAllocatable =
    "Amount of ephemeral-storage allocatable on the node.";
static constexpr const char *unitMetricK8sNodeEphemeralStorageAllocatable = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeEphemeralStorageAllocatable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeEphemeralStorageAllocatable,
                                         descrMetricK8sNodeEphemeralStorageAllocatable,
                                         unitMetricK8sNodeEphemeralStorageAllocatable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeEphemeralStorageAllocatable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeEphemeralStorageAllocatable,
                                          descrMetricK8sNodeEphemeralStorageAllocatable,
                                          unitMetricK8sNodeEphemeralStorageAllocatable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeEphemeralStorageAllocatable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeEphemeralStorageAllocatable,
                                                   descrMetricK8sNodeEphemeralStorageAllocatable,
                                                   unitMetricK8sNodeEphemeralStorageAllocatable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeEphemeralStorageAllocatable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeEphemeralStorageAllocatable,
                                                    descrMetricK8sNodeEphemeralStorageAllocatable,
                                                    unitMetricK8sNodeEphemeralStorageAllocatable);
}

/**
  Node filesystem available bytes.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#FsStats">FsStats.AvailableBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#NodeStats">NodeStats.Fs</a>
  of the Kubelet's stats API.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sNodeFilesystemAvailable = "k8s.node.filesystem.available";
static constexpr const char *descrMetricK8sNodeFilesystemAvailable =
    "Node filesystem available bytes.";
static constexpr const char *unitMetricK8sNodeFilesystemAvailable = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeFilesystemAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeFilesystemAvailable,
                                         descrMetricK8sNodeFilesystemAvailable,
                                         unitMetricK8sNodeFilesystemAvailable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeFilesystemAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeFilesystemAvailable,
                                          descrMetricK8sNodeFilesystemAvailable,
                                          unitMetricK8sNodeFilesystemAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeFilesystemAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeFilesystemAvailable,
                                                   descrMetricK8sNodeFilesystemAvailable,
                                                   unitMetricK8sNodeFilesystemAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeFilesystemAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeFilesystemAvailable,
                                                    descrMetricK8sNodeFilesystemAvailable,
                                                    unitMetricK8sNodeFilesystemAvailable);
}

/**
  Node filesystem capacity.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#FsStats">FsStats.CapacityBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#NodeStats">NodeStats.Fs</a>
  of the Kubelet's stats API.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sNodeFilesystemCapacity     = "k8s.node.filesystem.capacity";
static constexpr const char *descrMetricK8sNodeFilesystemCapacity = "Node filesystem capacity.";
static constexpr const char *unitMetricK8sNodeFilesystemCapacity  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeFilesystemCapacity(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeFilesystemCapacity,
                                         descrMetricK8sNodeFilesystemCapacity,
                                         unitMetricK8sNodeFilesystemCapacity);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeFilesystemCapacity(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeFilesystemCapacity,
                                          descrMetricK8sNodeFilesystemCapacity,
                                          unitMetricK8sNodeFilesystemCapacity);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeFilesystemCapacity(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeFilesystemCapacity,
                                                   descrMetricK8sNodeFilesystemCapacity,
                                                   unitMetricK8sNodeFilesystemCapacity);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeFilesystemCapacity(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeFilesystemCapacity,
                                                    descrMetricK8sNodeFilesystemCapacity,
                                                    unitMetricK8sNodeFilesystemCapacity);
}

/**
  Node filesystem usage.
  <p>
  This may not equal capacity - available.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#FsStats">FsStats.UsedBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#NodeStats">NodeStats.Fs</a>
  of the Kubelet's stats API.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sNodeFilesystemUsage     = "k8s.node.filesystem.usage";
static constexpr const char *descrMetricK8sNodeFilesystemUsage = "Node filesystem usage.";
static constexpr const char *unitMetricK8sNodeFilesystemUsage  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeFilesystemUsage,
                                         descrMetricK8sNodeFilesystemUsage,
                                         unitMetricK8sNodeFilesystemUsage);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeFilesystemUsage,
                                          descrMetricK8sNodeFilesystemUsage,
                                          unitMetricK8sNodeFilesystemUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeFilesystemUsage,
                                                   descrMetricK8sNodeFilesystemUsage,
                                                   unitMetricK8sNodeFilesystemUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeFilesystemUsage,
                                                    descrMetricK8sNodeFilesystemUsage,
                                                    unitMetricK8sNodeFilesystemUsage);
}

/**
  Amount of memory allocatable on the node.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sNodeMemoryAllocatable = "k8s.node.memory.allocatable";
static constexpr const char *descrMetricK8sNodeMemoryAllocatable =
    "Amount of memory allocatable on the node.";
static constexpr const char *unitMetricK8sNodeMemoryAllocatable = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeMemoryAllocatable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeMemoryAllocatable,
                                         descrMetricK8sNodeMemoryAllocatable,
                                         unitMetricK8sNodeMemoryAllocatable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeMemoryAllocatable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeMemoryAllocatable,
                                          descrMetricK8sNodeMemoryAllocatable,
                                          unitMetricK8sNodeMemoryAllocatable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeMemoryAllocatable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeMemoryAllocatable,
                                                   descrMetricK8sNodeMemoryAllocatable,
                                                   unitMetricK8sNodeMemoryAllocatable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeMemoryAllocatable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeMemoryAllocatable,
                                                    descrMetricK8sNodeMemoryAllocatable,
                                                    unitMetricK8sNodeMemoryAllocatable);
}

/**
  Node memory available.
  <p>
  Available memory for use.  This is defined as the memory limit - workingSetBytes. If memory limit
  is undefined, the available bytes is omitted. This metric is derived from the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#MemoryStats">MemoryStats.AvailableBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#NodeStats">NodeStats.Memory</a>
  of the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sNodeMemoryAvailable     = "k8s.node.memory.available";
static constexpr const char *descrMetricK8sNodeMemoryAvailable = "Node memory available.";
static constexpr const char *unitMetricK8sNodeMemoryAvailable  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeMemoryAvailable,
                                         descrMetricK8sNodeMemoryAvailable,
                                         unitMetricK8sNodeMemoryAvailable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeMemoryAvailable,
                                          descrMetricK8sNodeMemoryAvailable,
                                          unitMetricK8sNodeMemoryAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeMemoryAvailable,
                                                   descrMetricK8sNodeMemoryAvailable,
                                                   unitMetricK8sNodeMemoryAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeMemoryAvailable,
                                                    descrMetricK8sNodeMemoryAvailable,
                                                    unitMetricK8sNodeMemoryAvailable);
}

/**
  Node memory paging faults.
  <p>
  Cumulative number of major/minor page faults.
  This metric is derived from the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#MemoryStats">MemoryStats.PageFaults</a>
  and <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#MemoryStats">MemoryStats.MajorPageFaults</a>
  fields of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#NodeStats">NodeStats.Memory</a>
  of the Kubelet's stats API. <p> counter
 */
static constexpr const char *kMetricK8sNodeMemoryPagingFaults     = "k8s.node.memory.paging.faults";
static constexpr const char *descrMetricK8sNodeMemoryPagingFaults = "Node memory paging faults.";
static constexpr const char *unitMetricK8sNodeMemoryPagingFaults  = "{fault}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricK8sNodeMemoryPagingFaults(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricK8sNodeMemoryPagingFaults,
                                    descrMetricK8sNodeMemoryPagingFaults,
                                    unitMetricK8sNodeMemoryPagingFaults);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricK8sNodeMemoryPagingFaults(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricK8sNodeMemoryPagingFaults,
                                    descrMetricK8sNodeMemoryPagingFaults,
                                    unitMetricK8sNodeMemoryPagingFaults);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeMemoryPagingFaults(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricK8sNodeMemoryPagingFaults,
                                             descrMetricK8sNodeMemoryPagingFaults,
                                             unitMetricK8sNodeMemoryPagingFaults);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeMemoryPagingFaults(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricK8sNodeMemoryPagingFaults,
                                              descrMetricK8sNodeMemoryPagingFaults,
                                              unitMetricK8sNodeMemoryPagingFaults);
}

/**
  Node memory RSS.
  <p>
  The amount of anonymous and swap cache memory (includes transparent hugepages).
  This metric is derived from the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#MemoryStats">MemoryStats.RSSBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#NodeStats">NodeStats.Memory</a>
  of the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sNodeMemoryRss     = "k8s.node.memory.rss";
static constexpr const char *descrMetricK8sNodeMemoryRss = "Node memory RSS.";
static constexpr const char *unitMetricK8sNodeMemoryRss  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeMemoryRss(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeMemoryRss, descrMetricK8sNodeMemoryRss,
                                         unitMetricK8sNodeMemoryRss);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeMemoryRss(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeMemoryRss, descrMetricK8sNodeMemoryRss,
                                          unitMetricK8sNodeMemoryRss);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeMemoryRss(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sNodeMemoryRss, descrMetricK8sNodeMemoryRss, unitMetricK8sNodeMemoryRss);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeMemoryRss(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sNodeMemoryRss, descrMetricK8sNodeMemoryRss, unitMetricK8sNodeMemoryRss);
}

/**
  Memory usage of the Node.
  <p>
  Total memory usage of the Node
  <p>
  gauge
 */
static constexpr const char *kMetricK8sNodeMemoryUsage     = "k8s.node.memory.usage";
static constexpr const char *descrMetricK8sNodeMemoryUsage = "Memory usage of the Node.";
static constexpr const char *unitMetricK8sNodeMemoryUsage  = "By";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricK8sNodeMemoryUsage(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sNodeMemoryUsage, descrMetricK8sNodeMemoryUsage,
                                 unitMetricK8sNodeMemoryUsage);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricK8sNodeMemoryUsage(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sNodeMemoryUsage, descrMetricK8sNodeMemoryUsage,
                                  unitMetricK8sNodeMemoryUsage);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sNodeMemoryUsage, descrMetricK8sNodeMemoryUsage,
                                           unitMetricK8sNodeMemoryUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(
      kMetricK8sNodeMemoryUsage, descrMetricK8sNodeMemoryUsage, unitMetricK8sNodeMemoryUsage);
}

/**
  Node memory working set.
  <p>
  The amount of working set memory. This includes recently accessed memory, dirty memory, and kernel
  memory. WorkingSetBytes is <= UsageBytes. This metric is derived from the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#MemoryStats">MemoryStats.WorkingSetBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#NodeStats">NodeStats.Memory</a>
  of the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sNodeMemoryWorkingSet     = "k8s.node.memory.working_set";
static constexpr const char *descrMetricK8sNodeMemoryWorkingSet = "Node memory working set.";
static constexpr const char *unitMetricK8sNodeMemoryWorkingSet  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodeMemoryWorkingSet(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodeMemoryWorkingSet,
                                         descrMetricK8sNodeMemoryWorkingSet,
                                         unitMetricK8sNodeMemoryWorkingSet);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodeMemoryWorkingSet(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodeMemoryWorkingSet,
                                          descrMetricK8sNodeMemoryWorkingSet,
                                          unitMetricK8sNodeMemoryWorkingSet);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeMemoryWorkingSet(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodeMemoryWorkingSet,
                                                   descrMetricK8sNodeMemoryWorkingSet,
                                                   unitMetricK8sNodeMemoryWorkingSet);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeMemoryWorkingSet(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodeMemoryWorkingSet,
                                                    descrMetricK8sNodeMemoryWorkingSet,
                                                    unitMetricK8sNodeMemoryWorkingSet);
}

/**
  Node network errors.
  <p>
  counter
 */
static constexpr const char *kMetricK8sNodeNetworkErrors     = "k8s.node.network.errors";
static constexpr const char *descrMetricK8sNodeNetworkErrors = "Node network errors.";
static constexpr const char *unitMetricK8sNodeNetworkErrors  = "{error}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricK8sNodeNetworkErrors(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricK8sNodeNetworkErrors, descrMetricK8sNodeNetworkErrors,
                                    unitMetricK8sNodeNetworkErrors);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricK8sNodeNetworkErrors(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricK8sNodeNetworkErrors, descrMetricK8sNodeNetworkErrors,
                                    unitMetricK8sNodeNetworkErrors);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeNetworkErrors(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(
      kMetricK8sNodeNetworkErrors, descrMetricK8sNodeNetworkErrors, unitMetricK8sNodeNetworkErrors);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeNetworkErrors(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricK8sNodeNetworkErrors, descrMetricK8sNodeNetworkErrors, unitMetricK8sNodeNetworkErrors);
}

/**
  Network bytes for the Node.
  <p>
  counter
 */
static constexpr const char *kMetricK8sNodeNetworkIo     = "k8s.node.network.io";
static constexpr const char *descrMetricK8sNodeNetworkIo = "Network bytes for the Node.";
static constexpr const char *unitMetricK8sNodeNetworkIo  = "By";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricK8sNodeNetworkIo(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricK8sNodeNetworkIo, descrMetricK8sNodeNetworkIo,
                                    unitMetricK8sNodeNetworkIo);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricK8sNodeNetworkIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricK8sNodeNetworkIo, descrMetricK8sNodeNetworkIo,
                                    unitMetricK8sNodeNetworkIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodeNetworkIo(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricK8sNodeNetworkIo, descrMetricK8sNodeNetworkIo,
                                             unitMetricK8sNodeNetworkIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodeNetworkIo(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricK8sNodeNetworkIo, descrMetricK8sNodeNetworkIo,
                                              unitMetricK8sNodeNetworkIo);
}

/**
  Amount of pods allocatable on the node.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sNodePodAllocatable = "k8s.node.pod.allocatable";
static constexpr const char *descrMetricK8sNodePodAllocatable =
    "Amount of pods allocatable on the node.";
static constexpr const char *unitMetricK8sNodePodAllocatable = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sNodePodAllocatable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sNodePodAllocatable,
                                         descrMetricK8sNodePodAllocatable,
                                         unitMetricK8sNodePodAllocatable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sNodePodAllocatable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sNodePodAllocatable,
                                          descrMetricK8sNodePodAllocatable,
                                          unitMetricK8sNodePodAllocatable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sNodePodAllocatable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sNodePodAllocatable,
                                                   descrMetricK8sNodePodAllocatable,
                                                   unitMetricK8sNodePodAllocatable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sNodePodAllocatable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sNodePodAllocatable,
                                                    descrMetricK8sNodePodAllocatable,
                                                    unitMetricK8sNodePodAllocatable);
}

/**
  The time the Node has been running.
  <p>
  Instrumentations SHOULD use a gauge with type @code double @endcode and measure uptime in seconds
  as a floating point number with the highest precision available. The actual accuracy would depend
  on the instrumentation and operating system. <p> gauge
 */
static constexpr const char *kMetricK8sNodeUptime     = "k8s.node.uptime";
static constexpr const char *descrMetricK8sNodeUptime = "The time the Node has been running.";
static constexpr const char *unitMetricK8sNodeUptime  = "s";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricK8sNodeUptime(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sNodeUptime, descrMetricK8sNodeUptime,
                                 unitMetricK8sNodeUptime);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricK8sNodeUptime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sNodeUptime, descrMetricK8sNodeUptime,
                                  unitMetricK8sNodeUptime);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricK8sNodeUptime(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sNodeUptime, descrMetricK8sNodeUptime,
                                           unitMetricK8sNodeUptime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricK8sNodeUptime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricK8sNodeUptime, descrMetricK8sNodeUptime,
                                            unitMetricK8sNodeUptime);
}

/**
  Total CPU time consumed.
  <p>
  Total CPU time consumed by the specific Pod on all available CPU cores
  <p>
  counter
 */
static constexpr const char *kMetricK8sPodCpuTime     = "k8s.pod.cpu.time";
static constexpr const char *descrMetricK8sPodCpuTime = "Total CPU time consumed.";
static constexpr const char *unitMetricK8sPodCpuTime  = "s";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricK8sPodCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricK8sPodCpuTime, descrMetricK8sPodCpuTime,
                                    unitMetricK8sPodCpuTime);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricK8sPodCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricK8sPodCpuTime, descrMetricK8sPodCpuTime,
                                    unitMetricK8sPodCpuTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricK8sPodCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricK8sPodCpuTime, descrMetricK8sPodCpuTime,
                                             unitMetricK8sPodCpuTime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricK8sPodCpuTime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricK8sPodCpuTime, descrMetricK8sPodCpuTime,
                                              unitMetricK8sPodCpuTime);
}

/**
  Pod's CPU usage, measured in cpus. Range from 0 to the number of allocatable CPUs.
  <p>
  CPU usage of the specific Pod on all available CPU cores, averaged over the sample window
  <p>
  gauge
 */
static constexpr const char *kMetricK8sPodCpuUsage = "k8s.pod.cpu.usage";
static constexpr const char *descrMetricK8sPodCpuUsage =
    "Pod's CPU usage, measured in cpus. Range from 0 to the number of allocatable CPUs.";
static constexpr const char *unitMetricK8sPodCpuUsage = "{cpu}";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricK8sPodCpuUsage(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sPodCpuUsage, descrMetricK8sPodCpuUsage,
                                 unitMetricK8sPodCpuUsage);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricK8sPodCpuUsage(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sPodCpuUsage, descrMetricK8sPodCpuUsage,
                                  unitMetricK8sPodCpuUsage);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricK8sPodCpuUsage(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sPodCpuUsage, descrMetricK8sPodCpuUsage,
                                           unitMetricK8sPodCpuUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodCpuUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricK8sPodCpuUsage, descrMetricK8sPodCpuUsage,
                                            unitMetricK8sPodCpuUsage);
}

/**
  Pod filesystem available bytes.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#FsStats">FsStats.AvailableBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#PodStats">PodStats.EphemeralStorage</a>
  of the Kubelet's stats API.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sPodFilesystemAvailable = "k8s.pod.filesystem.available";
static constexpr const char *descrMetricK8sPodFilesystemAvailable =
    "Pod filesystem available bytes.";
static constexpr const char *unitMetricK8sPodFilesystemAvailable = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodFilesystemAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodFilesystemAvailable,
                                         descrMetricK8sPodFilesystemAvailable,
                                         unitMetricK8sPodFilesystemAvailable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodFilesystemAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodFilesystemAvailable,
                                          descrMetricK8sPodFilesystemAvailable,
                                          unitMetricK8sPodFilesystemAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodFilesystemAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sPodFilesystemAvailable,
                                                   descrMetricK8sPodFilesystemAvailable,
                                                   unitMetricK8sPodFilesystemAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodFilesystemAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sPodFilesystemAvailable,
                                                    descrMetricK8sPodFilesystemAvailable,
                                                    unitMetricK8sPodFilesystemAvailable);
}

/**
  Pod filesystem capacity.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#FsStats">FsStats.CapacityBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#PodStats">PodStats.EphemeralStorage</a>
  of the Kubelet's stats API.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sPodFilesystemCapacity     = "k8s.pod.filesystem.capacity";
static constexpr const char *descrMetricK8sPodFilesystemCapacity = "Pod filesystem capacity.";
static constexpr const char *unitMetricK8sPodFilesystemCapacity  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodFilesystemCapacity(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodFilesystemCapacity,
                                         descrMetricK8sPodFilesystemCapacity,
                                         unitMetricK8sPodFilesystemCapacity);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodFilesystemCapacity(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodFilesystemCapacity,
                                          descrMetricK8sPodFilesystemCapacity,
                                          unitMetricK8sPodFilesystemCapacity);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodFilesystemCapacity(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sPodFilesystemCapacity,
                                                   descrMetricK8sPodFilesystemCapacity,
                                                   unitMetricK8sPodFilesystemCapacity);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodFilesystemCapacity(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sPodFilesystemCapacity,
                                                    descrMetricK8sPodFilesystemCapacity,
                                                    unitMetricK8sPodFilesystemCapacity);
}

/**
  Pod filesystem usage.
  <p>
  This may not equal capacity - available.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#FsStats">FsStats.UsedBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#PodStats">PodStats.EphemeralStorage</a>
  of the Kubelet's stats API.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sPodFilesystemUsage     = "k8s.pod.filesystem.usage";
static constexpr const char *descrMetricK8sPodFilesystemUsage = "Pod filesystem usage.";
static constexpr const char *unitMetricK8sPodFilesystemUsage  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodFilesystemUsage,
                                         descrMetricK8sPodFilesystemUsage,
                                         unitMetricK8sPodFilesystemUsage);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodFilesystemUsage,
                                          descrMetricK8sPodFilesystemUsage,
                                          unitMetricK8sPodFilesystemUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sPodFilesystemUsage,
                                                   descrMetricK8sPodFilesystemUsage,
                                                   unitMetricK8sPodFilesystemUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodFilesystemUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sPodFilesystemUsage,
                                                    descrMetricK8sPodFilesystemUsage,
                                                    unitMetricK8sPodFilesystemUsage);
}

/**
  Pod memory available.
  <p>
  Available memory for use.  This is defined as the memory limit - workingSetBytes. If memory limit
  is undefined, the available bytes is omitted. This metric is derived from the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#MemoryStats">MemoryStats.AvailableBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#PodStats">PodStats.Memory</a>
  of the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sPodMemoryAvailable     = "k8s.pod.memory.available";
static constexpr const char *descrMetricK8sPodMemoryAvailable = "Pod memory available.";
static constexpr const char *unitMetricK8sPodMemoryAvailable  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodMemoryAvailable,
                                         descrMetricK8sPodMemoryAvailable,
                                         unitMetricK8sPodMemoryAvailable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodMemoryAvailable,
                                          descrMetricK8sPodMemoryAvailable,
                                          unitMetricK8sPodMemoryAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sPodMemoryAvailable,
                                                   descrMetricK8sPodMemoryAvailable,
                                                   unitMetricK8sPodMemoryAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodMemoryAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sPodMemoryAvailable,
                                                    descrMetricK8sPodMemoryAvailable,
                                                    unitMetricK8sPodMemoryAvailable);
}

/**
  Pod memory paging faults.
  <p>
  Cumulative number of major/minor page faults.
  This metric is derived from the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#MemoryStats">MemoryStats.PageFaults</a>
  and <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#MemoryStats">MemoryStats.MajorPageFaults</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#PodStats">PodStats.Memory</a>
  of the Kubelet's stats API. <p> counter
 */
static constexpr const char *kMetricK8sPodMemoryPagingFaults     = "k8s.pod.memory.paging.faults";
static constexpr const char *descrMetricK8sPodMemoryPagingFaults = "Pod memory paging faults.";
static constexpr const char *unitMetricK8sPodMemoryPagingFaults  = "{fault}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricK8sPodMemoryPagingFaults(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricK8sPodMemoryPagingFaults,
                                    descrMetricK8sPodMemoryPagingFaults,
                                    unitMetricK8sPodMemoryPagingFaults);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricK8sPodMemoryPagingFaults(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricK8sPodMemoryPagingFaults,
                                    descrMetricK8sPodMemoryPagingFaults,
                                    unitMetricK8sPodMemoryPagingFaults);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodMemoryPagingFaults(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricK8sPodMemoryPagingFaults,
                                             descrMetricK8sPodMemoryPagingFaults,
                                             unitMetricK8sPodMemoryPagingFaults);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodMemoryPagingFaults(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricK8sPodMemoryPagingFaults,
                                              descrMetricK8sPodMemoryPagingFaults,
                                              unitMetricK8sPodMemoryPagingFaults);
}

/**
  Pod memory RSS.
  <p>
  The amount of anonymous and swap cache memory (includes transparent hugepages).
  This metric is derived from the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#MemoryStats">MemoryStats.RSSBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#PodStats">PodStats.Memory</a>
  of the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sPodMemoryRss     = "k8s.pod.memory.rss";
static constexpr const char *descrMetricK8sPodMemoryRss = "Pod memory RSS.";
static constexpr const char *unitMetricK8sPodMemoryRss  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodMemoryRss(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodMemoryRss, descrMetricK8sPodMemoryRss,
                                         unitMetricK8sPodMemoryRss);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodMemoryRss(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodMemoryRss, descrMetricK8sPodMemoryRss,
                                          unitMetricK8sPodMemoryRss);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodMemoryRss(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sPodMemoryRss, descrMetricK8sPodMemoryRss, unitMetricK8sPodMemoryRss);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodMemoryRss(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sPodMemoryRss, descrMetricK8sPodMemoryRss, unitMetricK8sPodMemoryRss);
}

/**
  Memory usage of the Pod.
  <p>
  Total memory usage of the Pod
  <p>
  gauge
 */
static constexpr const char *kMetricK8sPodMemoryUsage     = "k8s.pod.memory.usage";
static constexpr const char *descrMetricK8sPodMemoryUsage = "Memory usage of the Pod.";
static constexpr const char *unitMetricK8sPodMemoryUsage  = "By";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricK8sPodMemoryUsage(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sPodMemoryUsage, descrMetricK8sPodMemoryUsage,
                                 unitMetricK8sPodMemoryUsage);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricK8sPodMemoryUsage(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sPodMemoryUsage, descrMetricK8sPodMemoryUsage,
                                  unitMetricK8sPodMemoryUsage);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sPodMemoryUsage, descrMetricK8sPodMemoryUsage,
                                           unitMetricK8sPodMemoryUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodMemoryUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricK8sPodMemoryUsage, descrMetricK8sPodMemoryUsage,
                                            unitMetricK8sPodMemoryUsage);
}

/**
  Pod memory working set.
  <p>
  The amount of working set memory. This includes recently accessed memory, dirty memory, and kernel
  memory. WorkingSetBytes is <= UsageBytes. This metric is derived from the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#MemoryStats">MemoryStats.WorkingSetBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.34.0/pkg/apis/stats/v1alpha1#PodStats">PodStats.Memory</a>
  of the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sPodMemoryWorkingSet     = "k8s.pod.memory.working_set";
static constexpr const char *descrMetricK8sPodMemoryWorkingSet = "Pod memory working set.";
static constexpr const char *unitMetricK8sPodMemoryWorkingSet  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodMemoryWorkingSet(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodMemoryWorkingSet,
                                         descrMetricK8sPodMemoryWorkingSet,
                                         unitMetricK8sPodMemoryWorkingSet);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodMemoryWorkingSet(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodMemoryWorkingSet,
                                          descrMetricK8sPodMemoryWorkingSet,
                                          unitMetricK8sPodMemoryWorkingSet);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodMemoryWorkingSet(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sPodMemoryWorkingSet,
                                                   descrMetricK8sPodMemoryWorkingSet,
                                                   unitMetricK8sPodMemoryWorkingSet);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodMemoryWorkingSet(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sPodMemoryWorkingSet,
                                                    descrMetricK8sPodMemoryWorkingSet,
                                                    unitMetricK8sPodMemoryWorkingSet);
}

/**
  Pod network errors.
  <p>
  counter
 */
static constexpr const char *kMetricK8sPodNetworkErrors     = "k8s.pod.network.errors";
static constexpr const char *descrMetricK8sPodNetworkErrors = "Pod network errors.";
static constexpr const char *unitMetricK8sPodNetworkErrors  = "{error}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricK8sPodNetworkErrors(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricK8sPodNetworkErrors, descrMetricK8sPodNetworkErrors,
                                    unitMetricK8sPodNetworkErrors);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricK8sPodNetworkErrors(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricK8sPodNetworkErrors, descrMetricK8sPodNetworkErrors,
                                    unitMetricK8sPodNetworkErrors);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodNetworkErrors(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(
      kMetricK8sPodNetworkErrors, descrMetricK8sPodNetworkErrors, unitMetricK8sPodNetworkErrors);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodNetworkErrors(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricK8sPodNetworkErrors, descrMetricK8sPodNetworkErrors, unitMetricK8sPodNetworkErrors);
}

/**
  Network bytes for the Pod.
  <p>
  counter
 */
static constexpr const char *kMetricK8sPodNetworkIo     = "k8s.pod.network.io";
static constexpr const char *descrMetricK8sPodNetworkIo = "Network bytes for the Pod.";
static constexpr const char *unitMetricK8sPodNetworkIo  = "By";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricK8sPodNetworkIo(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricK8sPodNetworkIo, descrMetricK8sPodNetworkIo,
                                    unitMetricK8sPodNetworkIo);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricK8sPodNetworkIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricK8sPodNetworkIo, descrMetricK8sPodNetworkIo,
                                    unitMetricK8sPodNetworkIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodNetworkIo(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricK8sPodNetworkIo, descrMetricK8sPodNetworkIo,
                                             unitMetricK8sPodNetworkIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodNetworkIo(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricK8sPodNetworkIo, descrMetricK8sPodNetworkIo,
                                              unitMetricK8sPodNetworkIo);
}

/**
  Describes number of K8s Pods that are currently in a given phase.
  <p>
  All possible pod phases will be reported at each time interval to avoid missing metrics.
  Only the value corresponding to the current phase will be non-zero.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sPodStatusPhase = "k8s.pod.status.phase";
static constexpr const char *descrMetricK8sPodStatusPhase =
    "Describes number of K8s Pods that are currently in a given phase.";
static constexpr const char *unitMetricK8sPodStatusPhase = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodStatusPhase(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodStatusPhase, descrMetricK8sPodStatusPhase,
                                         unitMetricK8sPodStatusPhase);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodStatusPhase(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodStatusPhase, descrMetricK8sPodStatusPhase,
                                          unitMetricK8sPodStatusPhase);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodStatusPhase(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sPodStatusPhase, descrMetricK8sPodStatusPhase, unitMetricK8sPodStatusPhase);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodStatusPhase(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sPodStatusPhase, descrMetricK8sPodStatusPhase, unitMetricK8sPodStatusPhase);
}

/**
  Describes the number of K8s Pods that are currently in a state for a given reason.
  <p>
  All possible pod status reasons will be reported at each time interval to avoid missing metrics.
  Only the value corresponding to the current reason will be non-zero.
  <p>
  updowncounter
 */
static constexpr const char *kMetricK8sPodStatusReason = "k8s.pod.status.reason";
static constexpr const char *descrMetricK8sPodStatusReason =
    "Describes the number of K8s Pods that are currently in a state for a given reason.";
static constexpr const char *unitMetricK8sPodStatusReason = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodStatusReason(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodStatusReason, descrMetricK8sPodStatusReason,
                                         unitMetricK8sPodStatusReason);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodStatusReason(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodStatusReason, descrMetricK8sPodStatusReason,
                                          unitMetricK8sPodStatusReason);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodStatusReason(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sPodStatusReason, descrMetricK8sPodStatusReason, unitMetricK8sPodStatusReason);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodStatusReason(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sPodStatusReason, descrMetricK8sPodStatusReason, unitMetricK8sPodStatusReason);
}

/**
  The time the Pod has been running.
  <p>
  Instrumentations SHOULD use a gauge with type @code double @endcode and measure uptime in seconds
  as a floating point number with the highest precision available. The actual accuracy would depend
  on the instrumentation and operating system. <p> gauge
 */
static constexpr const char *kMetricK8sPodUptime     = "k8s.pod.uptime";
static constexpr const char *descrMetricK8sPodUptime = "The time the Pod has been running.";
static constexpr const char *unitMetricK8sPodUptime  = "s";

#if OPENTELEMETRY_ABI_VERSION_NO >= 2

static inline nostd::unique_ptr<metrics::Gauge<int64_t>> CreateSyncInt64MetricK8sPodUptime(
    metrics::Meter *meter)
{
  return meter->CreateInt64Gauge(kMetricK8sPodUptime, descrMetricK8sPodUptime,
                                 unitMetricK8sPodUptime);
}

static inline nostd::unique_ptr<metrics::Gauge<double>> CreateSyncDoubleMetricK8sPodUptime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleGauge(kMetricK8sPodUptime, descrMetricK8sPodUptime,
                                  unitMetricK8sPodUptime);
}
#endif /* OPENTELEMETRY_ABI_VERSION_NO */

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricK8sPodUptime(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableGauge(kMetricK8sPodUptime, descrMetricK8sPodUptime,
                                           unitMetricK8sPodUptime);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricK8sPodUptime(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableGauge(kMetricK8sPodUptime, descrMetricK8sPodUptime,
                                            unitMetricK8sPodUptime);
}

/**
  Pod volume storage space available.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#VolumeStats">VolumeStats.AvailableBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#PodStats">PodStats</a> of
  the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sPodVolumeAvailable = "k8s.pod.volume.available";
static constexpr const char *descrMetricK8sPodVolumeAvailable =
    "Pod volume storage space available.";
static constexpr const char *unitMetricK8sPodVolumeAvailable = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodVolumeAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodVolumeAvailable,
                                         descrMetricK8sPodVolumeAvailable,
                                         unitMetricK8sPodVolumeAvailable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodVolumeAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodVolumeAvailable,
                                          descrMetricK8sPodVolumeAvailable,
                                          unitMetricK8sPodVolumeAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodVolumeAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sPodVolumeAvailable,
                                                   descrMetricK8sPodVolumeAvailable,
                                                   unitMetricK8sPodVolumeAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodVolumeAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sPodVolumeAvailable,
                                                    descrMetricK8sPodVolumeAvailable,
                                                    unitMetricK8sPodVolumeAvailable);
}

/**
  Pod volume total capacity.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#VolumeStats">VolumeStats.CapacityBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#PodStats">PodStats</a> of
  the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sPodVolumeCapacity     = "k8s.pod.volume.capacity";
static constexpr const char *descrMetricK8sPodVolumeCapacity = "Pod volume total capacity.";
static constexpr const char *unitMetricK8sPodVolumeCapacity  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodVolumeCapacity(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricK8sPodVolumeCapacity, descrMetricK8sPodVolumeCapacity, unitMetricK8sPodVolumeCapacity);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodVolumeCapacity(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricK8sPodVolumeCapacity, descrMetricK8sPodVolumeCapacity, unitMetricK8sPodVolumeCapacity);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodVolumeCapacity(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sPodVolumeCapacity, descrMetricK8sPodVolumeCapacity, unitMetricK8sPodVolumeCapacity);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodVolumeCapacity(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sPodVolumeCapacity, descrMetricK8sPodVolumeCapacity, unitMetricK8sPodVolumeCapacity);
}

/**
  The total inodes in the filesystem of the Pod's volume.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#VolumeStats">VolumeStats.Inodes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#PodStats">PodStats</a> of
  the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sPodVolumeInodeCount = "k8s.pod.volume.inode.count";
static constexpr const char *descrMetricK8sPodVolumeInodeCount =
    "The total inodes in the filesystem of the Pod's volume.";
static constexpr const char *unitMetricK8sPodVolumeInodeCount = "{inode}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodVolumeInodeCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodVolumeInodeCount,
                                         descrMetricK8sPodVolumeInodeCount,
                                         unitMetricK8sPodVolumeInodeCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodVolumeInodeCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodVolumeInodeCount,
                                          descrMetricK8sPodVolumeInodeCount,
                                          unitMetricK8sPodVolumeInodeCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodVolumeInodeCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sPodVolumeInodeCount,
                                                   descrMetricK8sPodVolumeInodeCount,
                                                   unitMetricK8sPodVolumeInodeCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodVolumeInodeCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sPodVolumeInodeCount,
                                                    descrMetricK8sPodVolumeInodeCount,
                                                    unitMetricK8sPodVolumeInodeCount);
}

/**
  The free inodes in the filesystem of the Pod's volume.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#VolumeStats">VolumeStats.InodesFree</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#PodStats">PodStats</a> of
  the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sPodVolumeInodeFree = "k8s.pod.volume.inode.free";
static constexpr const char *descrMetricK8sPodVolumeInodeFree =
    "The free inodes in the filesystem of the Pod's volume.";
static constexpr const char *unitMetricK8sPodVolumeInodeFree = "{inode}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodVolumeInodeFree(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodVolumeInodeFree,
                                         descrMetricK8sPodVolumeInodeFree,
                                         unitMetricK8sPodVolumeInodeFree);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodVolumeInodeFree(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodVolumeInodeFree,
                                          descrMetricK8sPodVolumeInodeFree,
                                          unitMetricK8sPodVolumeInodeFree);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodVolumeInodeFree(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sPodVolumeInodeFree,
                                                   descrMetricK8sPodVolumeInodeFree,
                                                   unitMetricK8sPodVolumeInodeFree);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodVolumeInodeFree(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sPodVolumeInodeFree,
                                                    descrMetricK8sPodVolumeInodeFree,
                                                    unitMetricK8sPodVolumeInodeFree);
}

/**
  The inodes used by the filesystem of the Pod's volume.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#VolumeStats">VolumeStats.InodesUsed</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#PodStats">PodStats</a> of
  the Kubelet's stats API. <p> This may not be equal to @code inodes - free @endcode because
  filesystem may share inodes with other filesystems. <p> updowncounter
 */
static constexpr const char *kMetricK8sPodVolumeInodeUsed = "k8s.pod.volume.inode.used";
static constexpr const char *descrMetricK8sPodVolumeInodeUsed =
    "The inodes used by the filesystem of the Pod's volume.";
static constexpr const char *unitMetricK8sPodVolumeInodeUsed = "{inode}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodVolumeInodeUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodVolumeInodeUsed,
                                         descrMetricK8sPodVolumeInodeUsed,
                                         unitMetricK8sPodVolumeInodeUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodVolumeInodeUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodVolumeInodeUsed,
                                          descrMetricK8sPodVolumeInodeUsed,
                                          unitMetricK8sPodVolumeInodeUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodVolumeInodeUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sPodVolumeInodeUsed,
                                                   descrMetricK8sPodVolumeInodeUsed,
                                                   unitMetricK8sPodVolumeInodeUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodVolumeInodeUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sPodVolumeInodeUsed,
                                                    descrMetricK8sPodVolumeInodeUsed,
                                                    unitMetricK8sPodVolumeInodeUsed);
}

/**
  Pod volume usage.
  <p>
  This may not equal capacity - available.
  <p>
  This metric is derived from the
  <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#VolumeStats">VolumeStats.UsedBytes</a>
  field of the <a
  href="https://pkg.go.dev/k8s.io/kubelet@v0.33.0/pkg/apis/stats/v1alpha1#PodStats">PodStats</a> of
  the Kubelet's stats API. <p> updowncounter
 */
static constexpr const char *kMetricK8sPodVolumeUsage     = "k8s.pod.volume.usage";
static constexpr const char *descrMetricK8sPodVolumeUsage = "Pod volume usage.";
static constexpr const char *unitMetricK8sPodVolumeUsage  = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sPodVolumeUsage(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sPodVolumeUsage, descrMetricK8sPodVolumeUsage,
                                         unitMetricK8sPodVolumeUsage);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sPodVolumeUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sPodVolumeUsage, descrMetricK8sPodVolumeUsage,
                                          unitMetricK8sPodVolumeUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sPodVolumeUsage(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sPodVolumeUsage, descrMetricK8sPodVolumeUsage, unitMetricK8sPodVolumeUsage);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sPodVolumeUsage(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sPodVolumeUsage, descrMetricK8sPodVolumeUsage, unitMetricK8sPodVolumeUsage);
}

/**
  Deprecated, use @code k8s.replicaset.pod.available @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.replicaset.pod.available @endcode.", "reason": "renamed",
  "renamed_to": "k8s.replicaset.pod.available"} <p> This metric aligns with the @code
  availableReplicas @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicasetstatus-v1-apps">K8s
  ReplicaSetStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sReplicasetAvailablePods =
    "k8s.replicaset.available_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sReplicasetAvailablePods =
    "Deprecated, use `k8s.replicaset.pod.available` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sReplicasetAvailablePods =
    "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicasetAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicasetAvailablePods,
                                         descrMetricK8sReplicasetAvailablePods,
                                         unitMetricK8sReplicasetAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicasetAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicasetAvailablePods,
                                          descrMetricK8sReplicasetAvailablePods,
                                          unitMetricK8sReplicasetAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicasetAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicasetAvailablePods,
                                                   descrMetricK8sReplicasetAvailablePods,
                                                   unitMetricK8sReplicasetAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicasetAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicasetAvailablePods,
                                                    descrMetricK8sReplicasetAvailablePods,
                                                    unitMetricK8sReplicasetAvailablePods);
}

/**
  Deprecated, use @code k8s.replicaset.pod.desired @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.replicaset.pod.desired @endcode.", "reason": "renamed",
  "renamed_to": "k8s.replicaset.pod.desired"} <p> This metric aligns with the @code replicas
  @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicasetspec-v1-apps">K8s
  ReplicaSetSpec</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sReplicasetDesiredPods =
    "k8s.replicaset.desired_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sReplicasetDesiredPods =
    "Deprecated, use `k8s.replicaset.pod.desired` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sReplicasetDesiredPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicasetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicasetDesiredPods,
                                         descrMetricK8sReplicasetDesiredPods,
                                         unitMetricK8sReplicasetDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicasetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicasetDesiredPods,
                                          descrMetricK8sReplicasetDesiredPods,
                                          unitMetricK8sReplicasetDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicasetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicasetDesiredPods,
                                                   descrMetricK8sReplicasetDesiredPods,
                                                   unitMetricK8sReplicasetDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicasetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicasetDesiredPods,
                                                    descrMetricK8sReplicasetDesiredPods,
                                                    unitMetricK8sReplicasetDesiredPods);
}

/**
  Total number of available replica pods (ready for at least minReadySeconds) targeted by this
  replicaset. <p> This metric aligns with the @code availableReplicas @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicasetstatus-v1-apps">K8s
  ReplicaSetStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sReplicasetPodAvailable = "k8s.replicaset.pod.available";
static constexpr const char *descrMetricK8sReplicasetPodAvailable =
    "Total number of available replica pods (ready for at least minReadySeconds) targeted by this "
    "replicaset.";
static constexpr const char *unitMetricK8sReplicasetPodAvailable = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicasetPodAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicasetPodAvailable,
                                         descrMetricK8sReplicasetPodAvailable,
                                         unitMetricK8sReplicasetPodAvailable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicasetPodAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicasetPodAvailable,
                                          descrMetricK8sReplicasetPodAvailable,
                                          unitMetricK8sReplicasetPodAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicasetPodAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicasetPodAvailable,
                                                   descrMetricK8sReplicasetPodAvailable,
                                                   unitMetricK8sReplicasetPodAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicasetPodAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicasetPodAvailable,
                                                    descrMetricK8sReplicasetPodAvailable,
                                                    unitMetricK8sReplicasetPodAvailable);
}

/**
  Number of desired replica pods in this replicaset.
  <p>
  This metric aligns with the @code replicas @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicasetspec-v1-apps">K8s
  ReplicaSetSpec</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sReplicasetPodDesired = "k8s.replicaset.pod.desired";
static constexpr const char *descrMetricK8sReplicasetPodDesired =
    "Number of desired replica pods in this replicaset.";
static constexpr const char *unitMetricK8sReplicasetPodDesired = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicasetPodDesired(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicasetPodDesired,
                                         descrMetricK8sReplicasetPodDesired,
                                         unitMetricK8sReplicasetPodDesired);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicasetPodDesired(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicasetPodDesired,
                                          descrMetricK8sReplicasetPodDesired,
                                          unitMetricK8sReplicasetPodDesired);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicasetPodDesired(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicasetPodDesired,
                                                   descrMetricK8sReplicasetPodDesired,
                                                   unitMetricK8sReplicasetPodDesired);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicasetPodDesired(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicasetPodDesired,
                                                    descrMetricK8sReplicasetPodDesired,
                                                    unitMetricK8sReplicasetPodDesired);
}

/**
  Deprecated, use @code k8s.replicationcontroller.pod.available @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.replicationcontroller.pod.available @endcode.", "reason":
  "renamed", "renamed_to": "k8s.replicationcontroller.pod.available"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sReplicationControllerAvailablePods =
    "k8s.replication_controller.available_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricK8sReplicationControllerAvailablePods =
        "Deprecated, use `k8s.replicationcontroller.pod.available` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *unitMetricK8sReplicationControllerAvailablePods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicationControllerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicationControllerAvailablePods,
                                         descrMetricK8sReplicationControllerAvailablePods,
                                         unitMetricK8sReplicationControllerAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicationControllerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicationControllerAvailablePods,
                                          descrMetricK8sReplicationControllerAvailablePods,
                                          unitMetricK8sReplicationControllerAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicationControllerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicationControllerAvailablePods,
                                                   descrMetricK8sReplicationControllerAvailablePods,
                                                   unitMetricK8sReplicationControllerAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicationControllerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sReplicationControllerAvailablePods,
      descrMetricK8sReplicationControllerAvailablePods,
      unitMetricK8sReplicationControllerAvailablePods);
}

/**
  Deprecated, use @code k8s.replicationcontroller.pod.desired @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.replicationcontroller.pod.desired @endcode.", "reason": "renamed",
  "renamed_to": "k8s.replicationcontroller.pod.desired"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sReplicationControllerDesiredPods =
    "k8s.replication_controller.desired_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricK8sReplicationControllerDesiredPods =
        "Deprecated, use `k8s.replicationcontroller.pod.desired` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *unitMetricK8sReplicationControllerDesiredPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicationControllerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicationControllerDesiredPods,
                                         descrMetricK8sReplicationControllerDesiredPods,
                                         unitMetricK8sReplicationControllerDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicationControllerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicationControllerDesiredPods,
                                          descrMetricK8sReplicationControllerDesiredPods,
                                          unitMetricK8sReplicationControllerDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicationControllerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicationControllerDesiredPods,
                                                   descrMetricK8sReplicationControllerDesiredPods,
                                                   unitMetricK8sReplicationControllerDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicationControllerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicationControllerDesiredPods,
                                                    descrMetricK8sReplicationControllerDesiredPods,
                                                    unitMetricK8sReplicationControllerDesiredPods);
}

/**
  Deprecated, use @code k8s.replicationcontroller.pod.available @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.replicationcontroller.pod.available @endcode.", "reason":
  "renamed", "renamed_to": "k8s.replicationcontroller.pod.available"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sReplicationcontrollerAvailablePods =
    "k8s.replicationcontroller.available_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricK8sReplicationcontrollerAvailablePods =
        "Deprecated, use `k8s.replicationcontroller.pod.available` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *unitMetricK8sReplicationcontrollerAvailablePods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicationcontrollerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicationcontrollerAvailablePods,
                                         descrMetricK8sReplicationcontrollerAvailablePods,
                                         unitMetricK8sReplicationcontrollerAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicationcontrollerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicationcontrollerAvailablePods,
                                          descrMetricK8sReplicationcontrollerAvailablePods,
                                          unitMetricK8sReplicationcontrollerAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicationcontrollerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicationcontrollerAvailablePods,
                                                   descrMetricK8sReplicationcontrollerAvailablePods,
                                                   unitMetricK8sReplicationcontrollerAvailablePods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicationcontrollerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sReplicationcontrollerAvailablePods,
      descrMetricK8sReplicationcontrollerAvailablePods,
      unitMetricK8sReplicationcontrollerAvailablePods);
}

/**
  Deprecated, use @code k8s.replicationcontroller.pod.desired @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.replicationcontroller.pod.desired @endcode.", "reason": "renamed",
  "renamed_to": "k8s.replicationcontroller.pod.desired"} <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sReplicationcontrollerDesiredPods =
    "k8s.replicationcontroller.desired_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricK8sReplicationcontrollerDesiredPods =
        "Deprecated, use `k8s.replicationcontroller.pod.desired` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *unitMetricK8sReplicationcontrollerDesiredPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicationcontrollerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicationcontrollerDesiredPods,
                                         descrMetricK8sReplicationcontrollerDesiredPods,
                                         unitMetricK8sReplicationcontrollerDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicationcontrollerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicationcontrollerDesiredPods,
                                          descrMetricK8sReplicationcontrollerDesiredPods,
                                          unitMetricK8sReplicationcontrollerDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicationcontrollerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicationcontrollerDesiredPods,
                                                   descrMetricK8sReplicationcontrollerDesiredPods,
                                                   unitMetricK8sReplicationcontrollerDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicationcontrollerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicationcontrollerDesiredPods,
                                                    descrMetricK8sReplicationcontrollerDesiredPods,
                                                    unitMetricK8sReplicationcontrollerDesiredPods);
}

/**
  Total number of available replica pods (ready for at least minReadySeconds) targeted by this
  replication controller. <p> This metric aligns with the @code availableReplicas @endcode field of
  the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicationcontrollerstatus-v1-core">K8s
  ReplicationControllerStatus</a> <p> updowncounter
 */
static constexpr const char *kMetricK8sReplicationcontrollerPodAvailable =
    "k8s.replicationcontroller.pod.available";
static constexpr const char *descrMetricK8sReplicationcontrollerPodAvailable =
    "Total number of available replica pods (ready for at least minReadySeconds) targeted by this "
    "replication controller.";
static constexpr const char *unitMetricK8sReplicationcontrollerPodAvailable = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicationcontrollerPodAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicationcontrollerPodAvailable,
                                         descrMetricK8sReplicationcontrollerPodAvailable,
                                         unitMetricK8sReplicationcontrollerPodAvailable);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicationcontrollerPodAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicationcontrollerPodAvailable,
                                          descrMetricK8sReplicationcontrollerPodAvailable,
                                          unitMetricK8sReplicationcontrollerPodAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicationcontrollerPodAvailable(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicationcontrollerPodAvailable,
                                                   descrMetricK8sReplicationcontrollerPodAvailable,
                                                   unitMetricK8sReplicationcontrollerPodAvailable);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicationcontrollerPodAvailable(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicationcontrollerPodAvailable,
                                                    descrMetricK8sReplicationcontrollerPodAvailable,
                                                    unitMetricK8sReplicationcontrollerPodAvailable);
}

/**
  Number of desired replica pods in this replication controller.
  <p>
  This metric aligns with the @code replicas @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicationcontrollerspec-v1-core">K8s
  ReplicationControllerSpec</a> <p> updowncounter
 */
static constexpr const char *kMetricK8sReplicationcontrollerPodDesired =
    "k8s.replicationcontroller.pod.desired";
static constexpr const char *descrMetricK8sReplicationcontrollerPodDesired =
    "Number of desired replica pods in this replication controller.";
static constexpr const char *unitMetricK8sReplicationcontrollerPodDesired = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicationcontrollerPodDesired(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicationcontrollerPodDesired,
                                         descrMetricK8sReplicationcontrollerPodDesired,
                                         unitMetricK8sReplicationcontrollerPodDesired);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicationcontrollerPodDesired(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicationcontrollerPodDesired,
                                          descrMetricK8sReplicationcontrollerPodDesired,
                                          unitMetricK8sReplicationcontrollerPodDesired);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicationcontrollerPodDesired(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicationcontrollerPodDesired,
                                                   descrMetricK8sReplicationcontrollerPodDesired,
                                                   unitMetricK8sReplicationcontrollerPodDesired);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicationcontrollerPodDesired(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicationcontrollerPodDesired,
                                                    descrMetricK8sReplicationcontrollerPodDesired,
                                                    unitMetricK8sReplicationcontrollerPodDesired);
}

/**
  The CPU limits in a specific namespace.
  The value represents the configured quota limit of the resource in the namespace.
  <p>
  This metric is retrieved from the @code hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaCpuLimitHard =
    "k8s.resourcequota.cpu.limit.hard";
static constexpr const char *descrMetricK8sResourcequotaCpuLimitHard =
    "The CPU limits in a specific namespace.
    The value represents the configured quota limit of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaCpuLimitHard = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaCpuLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaCpuLimitHard,
                                         descrMetricK8sResourcequotaCpuLimitHard,
                                         unitMetricK8sResourcequotaCpuLimitHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaCpuLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaCpuLimitHard,
                                          descrMetricK8sResourcequotaCpuLimitHard,
                                          unitMetricK8sResourcequotaCpuLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaCpuLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaCpuLimitHard,
                                                   descrMetricK8sResourcequotaCpuLimitHard,
                                                   unitMetricK8sResourcequotaCpuLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaCpuLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaCpuLimitHard,
                                                    descrMetricK8sResourcequotaCpuLimitHard,
                                                    unitMetricK8sResourcequotaCpuLimitHard);
}

/**
  The CPU limits in a specific namespace.
  The value represents the current observed total usage of the resource in the namespace.
  <p>
  This metric is retrieved from the @code used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaCpuLimitUsed =
    "k8s.resourcequota.cpu.limit.used";
static constexpr const char *descrMetricK8sResourcequotaCpuLimitUsed =
    "The CPU limits in a specific namespace.
    The value represents the current observed total usage of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaCpuLimitUsed = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaCpuLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaCpuLimitUsed,
                                         descrMetricK8sResourcequotaCpuLimitUsed,
                                         unitMetricK8sResourcequotaCpuLimitUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaCpuLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaCpuLimitUsed,
                                          descrMetricK8sResourcequotaCpuLimitUsed,
                                          unitMetricK8sResourcequotaCpuLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaCpuLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaCpuLimitUsed,
                                                   descrMetricK8sResourcequotaCpuLimitUsed,
                                                   unitMetricK8sResourcequotaCpuLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaCpuLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaCpuLimitUsed,
                                                    descrMetricK8sResourcequotaCpuLimitUsed,
                                                    unitMetricK8sResourcequotaCpuLimitUsed);
}

/**
  The CPU requests in a specific namespace.
  The value represents the configured quota limit of the resource in the namespace.
  <p>
  This metric is retrieved from the @code hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaCpuRequestHard =
    "k8s.resourcequota.cpu.request.hard";
static constexpr const char *descrMetricK8sResourcequotaCpuRequestHard =
    "The CPU requests in a specific namespace.
    The value represents the configured quota limit of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaCpuRequestHard = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaCpuRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaCpuRequestHard,
                                         descrMetricK8sResourcequotaCpuRequestHard,
                                         unitMetricK8sResourcequotaCpuRequestHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaCpuRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaCpuRequestHard,
                                          descrMetricK8sResourcequotaCpuRequestHard,
                                          unitMetricK8sResourcequotaCpuRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaCpuRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaCpuRequestHard,
                                                   descrMetricK8sResourcequotaCpuRequestHard,
                                                   unitMetricK8sResourcequotaCpuRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaCpuRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaCpuRequestHard,
                                                    descrMetricK8sResourcequotaCpuRequestHard,
                                                    unitMetricK8sResourcequotaCpuRequestHard);
}

/**
  The CPU requests in a specific namespace.
  The value represents the current observed total usage of the resource in the namespace.
  <p>
  This metric is retrieved from the @code used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaCpuRequestUsed =
    "k8s.resourcequota.cpu.request.used";
static constexpr const char *descrMetricK8sResourcequotaCpuRequestUsed =
    "The CPU requests in a specific namespace.
    The value represents the current observed total usage of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaCpuRequestUsed = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaCpuRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaCpuRequestUsed,
                                         descrMetricK8sResourcequotaCpuRequestUsed,
                                         unitMetricK8sResourcequotaCpuRequestUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaCpuRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaCpuRequestUsed,
                                          descrMetricK8sResourcequotaCpuRequestUsed,
                                          unitMetricK8sResourcequotaCpuRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaCpuRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaCpuRequestUsed,
                                                   descrMetricK8sResourcequotaCpuRequestUsed,
                                                   unitMetricK8sResourcequotaCpuRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaCpuRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaCpuRequestUsed,
                                                    descrMetricK8sResourcequotaCpuRequestUsed,
                                                    unitMetricK8sResourcequotaCpuRequestUsed);
}

/**
  The sum of local ephemeral storage limits in the namespace.
  The value represents the configured quota limit of the resource in the namespace.
  <p>
  This metric is retrieved from the @code hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaEphemeralStorageLimitHard =
    "k8s.resourcequota.ephemeral_storage.limit.hard";
static constexpr const char *descrMetricK8sResourcequotaEphemeralStorageLimitHard =
    "The sum of local ephemeral storage limits in the namespace.
    The value represents the configured quota limit of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaEphemeralStorageLimitHard = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaEphemeralStorageLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaEphemeralStorageLimitHard,
                                         descrMetricK8sResourcequotaEphemeralStorageLimitHard,
                                         unitMetricK8sResourcequotaEphemeralStorageLimitHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaEphemeralStorageLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaEphemeralStorageLimitHard,
                                          descrMetricK8sResourcequotaEphemeralStorageLimitHard,
                                          unitMetricK8sResourcequotaEphemeralStorageLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaEphemeralStorageLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sResourcequotaEphemeralStorageLimitHard,
      descrMetricK8sResourcequotaEphemeralStorageLimitHard,
      unitMetricK8sResourcequotaEphemeralStorageLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaEphemeralStorageLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sResourcequotaEphemeralStorageLimitHard,
      descrMetricK8sResourcequotaEphemeralStorageLimitHard,
      unitMetricK8sResourcequotaEphemeralStorageLimitHard);
}

/**
  The sum of local ephemeral storage limits in the namespace.
  The value represents the current observed total usage of the resource in the namespace.
  <p>
  This metric is retrieved from the @code used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaEphemeralStorageLimitUsed =
    "k8s.resourcequota.ephemeral_storage.limit.used";
static constexpr const char *descrMetricK8sResourcequotaEphemeralStorageLimitUsed =
    "The sum of local ephemeral storage limits in the namespace.
    The value represents the current observed total usage of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaEphemeralStorageLimitUsed = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaEphemeralStorageLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaEphemeralStorageLimitUsed,
                                         descrMetricK8sResourcequotaEphemeralStorageLimitUsed,
                                         unitMetricK8sResourcequotaEphemeralStorageLimitUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaEphemeralStorageLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaEphemeralStorageLimitUsed,
                                          descrMetricK8sResourcequotaEphemeralStorageLimitUsed,
                                          unitMetricK8sResourcequotaEphemeralStorageLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaEphemeralStorageLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sResourcequotaEphemeralStorageLimitUsed,
      descrMetricK8sResourcequotaEphemeralStorageLimitUsed,
      unitMetricK8sResourcequotaEphemeralStorageLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaEphemeralStorageLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sResourcequotaEphemeralStorageLimitUsed,
      descrMetricK8sResourcequotaEphemeralStorageLimitUsed,
      unitMetricK8sResourcequotaEphemeralStorageLimitUsed);
}

/**
  The sum of local ephemeral storage requests in the namespace.
  The value represents the configured quota limit of the resource in the namespace.
  <p>
  This metric is retrieved from the @code hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaEphemeralStorageRequestHard =
    "k8s.resourcequota.ephemeral_storage.request.hard";
static constexpr const char *descrMetricK8sResourcequotaEphemeralStorageRequestHard =
    "The sum of local ephemeral storage requests in the namespace.
    The value represents the configured quota limit of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaEphemeralStorageRequestHard = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaEphemeralStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaEphemeralStorageRequestHard,
                                         descrMetricK8sResourcequotaEphemeralStorageRequestHard,
                                         unitMetricK8sResourcequotaEphemeralStorageRequestHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaEphemeralStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaEphemeralStorageRequestHard,
                                          descrMetricK8sResourcequotaEphemeralStorageRequestHard,
                                          unitMetricK8sResourcequotaEphemeralStorageRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaEphemeralStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sResourcequotaEphemeralStorageRequestHard,
      descrMetricK8sResourcequotaEphemeralStorageRequestHard,
      unitMetricK8sResourcequotaEphemeralStorageRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaEphemeralStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sResourcequotaEphemeralStorageRequestHard,
      descrMetricK8sResourcequotaEphemeralStorageRequestHard,
      unitMetricK8sResourcequotaEphemeralStorageRequestHard);
}

/**
  The sum of local ephemeral storage requests in the namespace.
  The value represents the current observed total usage of the resource in the namespace.
  <p>
  This metric is retrieved from the @code used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaEphemeralStorageRequestUsed =
    "k8s.resourcequota.ephemeral_storage.request.used";
static constexpr const char *descrMetricK8sResourcequotaEphemeralStorageRequestUsed =
    "The sum of local ephemeral storage requests in the namespace.
    The value represents the current observed total usage of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaEphemeralStorageRequestUsed = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaEphemeralStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaEphemeralStorageRequestUsed,
                                         descrMetricK8sResourcequotaEphemeralStorageRequestUsed,
                                         unitMetricK8sResourcequotaEphemeralStorageRequestUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaEphemeralStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaEphemeralStorageRequestUsed,
                                          descrMetricK8sResourcequotaEphemeralStorageRequestUsed,
                                          unitMetricK8sResourcequotaEphemeralStorageRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaEphemeralStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sResourcequotaEphemeralStorageRequestUsed,
      descrMetricK8sResourcequotaEphemeralStorageRequestUsed,
      unitMetricK8sResourcequotaEphemeralStorageRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaEphemeralStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sResourcequotaEphemeralStorageRequestUsed,
      descrMetricK8sResourcequotaEphemeralStorageRequestUsed,
      unitMetricK8sResourcequotaEphemeralStorageRequestUsed);
}

/**
  The huge page requests in a specific namespace.
  The value represents the configured quota limit of the resource in the namespace.
  <p>
  This metric is retrieved from the @code hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaHugepageCountRequestHard =
    "k8s.resourcequota.hugepage_count.request.hard";
static constexpr const char *descrMetricK8sResourcequotaHugepageCountRequestHard =
    "The huge page requests in a specific namespace.
    The value represents the configured quota limit of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaHugepageCountRequestHard = "{hugepage}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaHugepageCountRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaHugepageCountRequestHard,
                                         descrMetricK8sResourcequotaHugepageCountRequestHard,
                                         unitMetricK8sResourcequotaHugepageCountRequestHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaHugepageCountRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaHugepageCountRequestHard,
                                          descrMetricK8sResourcequotaHugepageCountRequestHard,
                                          unitMetricK8sResourcequotaHugepageCountRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaHugepageCountRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sResourcequotaHugepageCountRequestHard,
      descrMetricK8sResourcequotaHugepageCountRequestHard,
      unitMetricK8sResourcequotaHugepageCountRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaHugepageCountRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sResourcequotaHugepageCountRequestHard,
      descrMetricK8sResourcequotaHugepageCountRequestHard,
      unitMetricK8sResourcequotaHugepageCountRequestHard);
}

/**
  The huge page requests in a specific namespace.
  The value represents the current observed total usage of the resource in the namespace.
  <p>
  This metric is retrieved from the @code used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaHugepageCountRequestUsed =
    "k8s.resourcequota.hugepage_count.request.used";
static constexpr const char *descrMetricK8sResourcequotaHugepageCountRequestUsed =
    "The huge page requests in a specific namespace.
    The value represents the current observed total usage of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaHugepageCountRequestUsed = "{hugepage}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaHugepageCountRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaHugepageCountRequestUsed,
                                         descrMetricK8sResourcequotaHugepageCountRequestUsed,
                                         unitMetricK8sResourcequotaHugepageCountRequestUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaHugepageCountRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaHugepageCountRequestUsed,
                                          descrMetricK8sResourcequotaHugepageCountRequestUsed,
                                          unitMetricK8sResourcequotaHugepageCountRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaHugepageCountRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sResourcequotaHugepageCountRequestUsed,
      descrMetricK8sResourcequotaHugepageCountRequestUsed,
      unitMetricK8sResourcequotaHugepageCountRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaHugepageCountRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sResourcequotaHugepageCountRequestUsed,
      descrMetricK8sResourcequotaHugepageCountRequestUsed,
      unitMetricK8sResourcequotaHugepageCountRequestUsed);
}

/**
  The memory limits in a specific namespace.
  The value represents the configured quota limit of the resource in the namespace.
  <p>
  This metric is retrieved from the @code hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaMemoryLimitHard =
    "k8s.resourcequota.memory.limit.hard";
static constexpr const char *descrMetricK8sResourcequotaMemoryLimitHard =
    "The memory limits in a specific namespace.
    The value represents the configured quota limit of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaMemoryLimitHard = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaMemoryLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaMemoryLimitHard,
                                         descrMetricK8sResourcequotaMemoryLimitHard,
                                         unitMetricK8sResourcequotaMemoryLimitHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaMemoryLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaMemoryLimitHard,
                                          descrMetricK8sResourcequotaMemoryLimitHard,
                                          unitMetricK8sResourcequotaMemoryLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaMemoryLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaMemoryLimitHard,
                                                   descrMetricK8sResourcequotaMemoryLimitHard,
                                                   unitMetricK8sResourcequotaMemoryLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaMemoryLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaMemoryLimitHard,
                                                    descrMetricK8sResourcequotaMemoryLimitHard,
                                                    unitMetricK8sResourcequotaMemoryLimitHard);
}

/**
  The memory limits in a specific namespace.
  The value represents the current observed total usage of the resource in the namespace.
  <p>
  This metric is retrieved from the @code used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaMemoryLimitUsed =
    "k8s.resourcequota.memory.limit.used";
static constexpr const char *descrMetricK8sResourcequotaMemoryLimitUsed =
    "The memory limits in a specific namespace.
    The value represents the current observed total usage of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaMemoryLimitUsed = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaMemoryLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaMemoryLimitUsed,
                                         descrMetricK8sResourcequotaMemoryLimitUsed,
                                         unitMetricK8sResourcequotaMemoryLimitUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaMemoryLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaMemoryLimitUsed,
                                          descrMetricK8sResourcequotaMemoryLimitUsed,
                                          unitMetricK8sResourcequotaMemoryLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaMemoryLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaMemoryLimitUsed,
                                                   descrMetricK8sResourcequotaMemoryLimitUsed,
                                                   unitMetricK8sResourcequotaMemoryLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaMemoryLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaMemoryLimitUsed,
                                                    descrMetricK8sResourcequotaMemoryLimitUsed,
                                                    unitMetricK8sResourcequotaMemoryLimitUsed);
}

/**
  The memory requests in a specific namespace.
  The value represents the configured quota limit of the resource in the namespace.
  <p>
  This metric is retrieved from the @code hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaMemoryRequestHard =
    "k8s.resourcequota.memory.request.hard";
static constexpr const char *descrMetricK8sResourcequotaMemoryRequestHard =
    "The memory requests in a specific namespace.
    The value represents the configured quota limit of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaMemoryRequestHard = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaMemoryRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaMemoryRequestHard,
                                         descrMetricK8sResourcequotaMemoryRequestHard,
                                         unitMetricK8sResourcequotaMemoryRequestHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaMemoryRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaMemoryRequestHard,
                                          descrMetricK8sResourcequotaMemoryRequestHard,
                                          unitMetricK8sResourcequotaMemoryRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaMemoryRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaMemoryRequestHard,
                                                   descrMetricK8sResourcequotaMemoryRequestHard,
                                                   unitMetricK8sResourcequotaMemoryRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaMemoryRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaMemoryRequestHard,
                                                    descrMetricK8sResourcequotaMemoryRequestHard,
                                                    unitMetricK8sResourcequotaMemoryRequestHard);
}

/**
  The memory requests in a specific namespace.
  The value represents the current observed total usage of the resource in the namespace.
  <p>
  This metric is retrieved from the @code used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaMemoryRequestUsed =
    "k8s.resourcequota.memory.request.used";
static constexpr const char *descrMetricK8sResourcequotaMemoryRequestUsed =
    "The memory requests in a specific namespace.
    The value represents the current observed total usage of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaMemoryRequestUsed = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaMemoryRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaMemoryRequestUsed,
                                         descrMetricK8sResourcequotaMemoryRequestUsed,
                                         unitMetricK8sResourcequotaMemoryRequestUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaMemoryRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaMemoryRequestUsed,
                                          descrMetricK8sResourcequotaMemoryRequestUsed,
                                          unitMetricK8sResourcequotaMemoryRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaMemoryRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaMemoryRequestUsed,
                                                   descrMetricK8sResourcequotaMemoryRequestUsed,
                                                   unitMetricK8sResourcequotaMemoryRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaMemoryRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaMemoryRequestUsed,
                                                    descrMetricK8sResourcequotaMemoryRequestUsed,
                                                    unitMetricK8sResourcequotaMemoryRequestUsed);
}

/**
  The object count limits in a specific namespace.
  The value represents the configured quota limit of the resource in the namespace.
  <p>
  This metric is retrieved from the @code hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaObjectCountHard =
    "k8s.resourcequota.object_count.hard";
static constexpr const char *descrMetricK8sResourcequotaObjectCountHard =
    "The object count limits in a specific namespace.
    The value represents the configured quota limit of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaObjectCountHard = "{object}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaObjectCountHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaObjectCountHard,
                                         descrMetricK8sResourcequotaObjectCountHard,
                                         unitMetricK8sResourcequotaObjectCountHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaObjectCountHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaObjectCountHard,
                                          descrMetricK8sResourcequotaObjectCountHard,
                                          unitMetricK8sResourcequotaObjectCountHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaObjectCountHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaObjectCountHard,
                                                   descrMetricK8sResourcequotaObjectCountHard,
                                                   unitMetricK8sResourcequotaObjectCountHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaObjectCountHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaObjectCountHard,
                                                    descrMetricK8sResourcequotaObjectCountHard,
                                                    unitMetricK8sResourcequotaObjectCountHard);
}

/**
  The object count limits in a specific namespace.
  The value represents the current observed total usage of the resource in the namespace.
  <p>
  This metric is retrieved from the @code used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaObjectCountUsed =
    "k8s.resourcequota.object_count.used";
static constexpr const char *descrMetricK8sResourcequotaObjectCountUsed =
    "The object count limits in a specific namespace.
    The value represents the current observed total usage of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaObjectCountUsed = "{object}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaObjectCountUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaObjectCountUsed,
                                         descrMetricK8sResourcequotaObjectCountUsed,
                                         unitMetricK8sResourcequotaObjectCountUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaObjectCountUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaObjectCountUsed,
                                          descrMetricK8sResourcequotaObjectCountUsed,
                                          unitMetricK8sResourcequotaObjectCountUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaObjectCountUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaObjectCountUsed,
                                                   descrMetricK8sResourcequotaObjectCountUsed,
                                                   unitMetricK8sResourcequotaObjectCountUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaObjectCountUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaObjectCountUsed,
                                                    descrMetricK8sResourcequotaObjectCountUsed,
                                                    unitMetricK8sResourcequotaObjectCountUsed);
}

/**
  The total number of PersistentVolumeClaims that can exist in the namespace.
  The value represents the configured quota limit of the resource in the namespace.
  <p>
  This metric is retrieved from the @code hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> The @code k8s.storageclass.name @endcode should be required when a
  resource quota is defined for a specific storage class. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaPersistentvolumeclaimCountHard =
    "k8s.resourcequota.persistentvolumeclaim_count.hard";
static constexpr const char *descrMetricK8sResourcequotaPersistentvolumeclaimCountHard =
    "The total number of PersistentVolumeClaims that can exist in the namespace.
    The value represents the configured quota limit of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaPersistentvolumeclaimCountHard =
        "{persistentvolumeclaim}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaPersistentvolumeclaimCountHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaPersistentvolumeclaimCountHard,
                                         descrMetricK8sResourcequotaPersistentvolumeclaimCountHard,
                                         unitMetricK8sResourcequotaPersistentvolumeclaimCountHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaPersistentvolumeclaimCountHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaPersistentvolumeclaimCountHard,
                                          descrMetricK8sResourcequotaPersistentvolumeclaimCountHard,
                                          unitMetricK8sResourcequotaPersistentvolumeclaimCountHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaPersistentvolumeclaimCountHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sResourcequotaPersistentvolumeclaimCountHard,
      descrMetricK8sResourcequotaPersistentvolumeclaimCountHard,
      unitMetricK8sResourcequotaPersistentvolumeclaimCountHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaPersistentvolumeclaimCountHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sResourcequotaPersistentvolumeclaimCountHard,
      descrMetricK8sResourcequotaPersistentvolumeclaimCountHard,
      unitMetricK8sResourcequotaPersistentvolumeclaimCountHard);
}

/**
  The total number of PersistentVolumeClaims that can exist in the namespace.
  The value represents the current observed total usage of the resource in the namespace.
  <p>
  This metric is retrieved from the @code used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> The @code k8s.storageclass.name @endcode should be required when a
  resource quota is defined for a specific storage class. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaPersistentvolumeclaimCountUsed =
    "k8s.resourcequota.persistentvolumeclaim_count.used";
static constexpr const char *descrMetricK8sResourcequotaPersistentvolumeclaimCountUsed =
    "The total number of PersistentVolumeClaims that can exist in the namespace.
    The value represents the current observed total usage of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaPersistentvolumeclaimCountUsed =
        "{persistentvolumeclaim}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaPersistentvolumeclaimCountUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaPersistentvolumeclaimCountUsed,
                                         descrMetricK8sResourcequotaPersistentvolumeclaimCountUsed,
                                         unitMetricK8sResourcequotaPersistentvolumeclaimCountUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaPersistentvolumeclaimCountUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaPersistentvolumeclaimCountUsed,
                                          descrMetricK8sResourcequotaPersistentvolumeclaimCountUsed,
                                          unitMetricK8sResourcequotaPersistentvolumeclaimCountUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaPersistentvolumeclaimCountUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sResourcequotaPersistentvolumeclaimCountUsed,
      descrMetricK8sResourcequotaPersistentvolumeclaimCountUsed,
      unitMetricK8sResourcequotaPersistentvolumeclaimCountUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaPersistentvolumeclaimCountUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sResourcequotaPersistentvolumeclaimCountUsed,
      descrMetricK8sResourcequotaPersistentvolumeclaimCountUsed,
      unitMetricK8sResourcequotaPersistentvolumeclaimCountUsed);
}

/**
  The storage requests in a specific namespace.
  The value represents the configured quota limit of the resource in the namespace.
  <p>
  This metric is retrieved from the @code hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> The @code k8s.storageclass.name @endcode should be required when a
  resource quota is defined for a specific storage class. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaStorageRequestHard =
    "k8s.resourcequota.storage.request.hard";
static constexpr const char *descrMetricK8sResourcequotaStorageRequestHard =
    "The storage requests in a specific namespace.
    The value represents the configured quota limit of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaStorageRequestHard = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaStorageRequestHard,
                                         descrMetricK8sResourcequotaStorageRequestHard,
                                         unitMetricK8sResourcequotaStorageRequestHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaStorageRequestHard,
                                          descrMetricK8sResourcequotaStorageRequestHard,
                                          unitMetricK8sResourcequotaStorageRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaStorageRequestHard,
                                                   descrMetricK8sResourcequotaStorageRequestHard,
                                                   unitMetricK8sResourcequotaStorageRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaStorageRequestHard,
                                                    descrMetricK8sResourcequotaStorageRequestHard,
                                                    unitMetricK8sResourcequotaStorageRequestHard);
}

/**
  The storage requests in a specific namespace.
  The value represents the current observed total usage of the resource in the namespace.
  <p>
  This metric is retrieved from the @code used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a>. <p> The @code k8s.storageclass.name @endcode should be required when a
  resource quota is defined for a specific storage class. <p> updowncounter
 */
static constexpr const char *kMetricK8sResourcequotaStorageRequestUsed =
    "k8s.resourcequota.storage.request.used";
static constexpr const char *descrMetricK8sResourcequotaStorageRequestUsed =
    "The storage requests in a specific namespace.
    The value represents the current observed total usage of the resource in the namespace.";
    static constexpr const char *unitMetricK8sResourcequotaStorageRequestUsed = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sResourcequotaStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sResourcequotaStorageRequestUsed,
                                         descrMetricK8sResourcequotaStorageRequestUsed,
                                         unitMetricK8sResourcequotaStorageRequestUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sResourcequotaStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sResourcequotaStorageRequestUsed,
                                          descrMetricK8sResourcequotaStorageRequestUsed,
                                          unitMetricK8sResourcequotaStorageRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sResourcequotaStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sResourcequotaStorageRequestUsed,
                                                   descrMetricK8sResourcequotaStorageRequestUsed,
                                                   unitMetricK8sResourcequotaStorageRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sResourcequotaStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sResourcequotaStorageRequestUsed,
                                                    descrMetricK8sResourcequotaStorageRequestUsed,
                                                    unitMetricK8sResourcequotaStorageRequestUsed);
}

/**
  Deprecated, use @code k8s.statefulset.pod.current @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.statefulset.pod.current @endcode.", "reason": "renamed",
  "renamed_to": "k8s.statefulset.pod.current"} <p> This metric aligns with the @code currentReplicas
  @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetstatus-v1-apps">K8s
  StatefulSetStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sStatefulsetCurrentPods =
    "k8s.statefulset.current_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sStatefulsetCurrentPods =
    "Deprecated, use `k8s.statefulset.pod.current` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sStatefulsetCurrentPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetCurrentPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetCurrentPods,
                                         descrMetricK8sStatefulsetCurrentPods,
                                         unitMetricK8sStatefulsetCurrentPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetCurrentPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetCurrentPods,
                                          descrMetricK8sStatefulsetCurrentPods,
                                          unitMetricK8sStatefulsetCurrentPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetCurrentPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetCurrentPods,
                                                   descrMetricK8sStatefulsetCurrentPods,
                                                   unitMetricK8sStatefulsetCurrentPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetCurrentPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetCurrentPods,
                                                    descrMetricK8sStatefulsetCurrentPods,
                                                    unitMetricK8sStatefulsetCurrentPods);
}

/**
  Deprecated, use @code k8s.statefulset.pod.desired @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.statefulset.pod.desired @endcode.", "reason": "renamed",
  "renamed_to": "k8s.statefulset.pod.desired"} <p> This metric aligns with the @code replicas
  @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetspec-v1-apps">K8s
  StatefulSetSpec</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sStatefulsetDesiredPods =
    "k8s.statefulset.desired_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sStatefulsetDesiredPods =
    "Deprecated, use `k8s.statefulset.pod.desired` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sStatefulsetDesiredPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetDesiredPods,
                                         descrMetricK8sStatefulsetDesiredPods,
                                         unitMetricK8sStatefulsetDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetDesiredPods,
                                          descrMetricK8sStatefulsetDesiredPods,
                                          unitMetricK8sStatefulsetDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetDesiredPods,
                                                   descrMetricK8sStatefulsetDesiredPods,
                                                   unitMetricK8sStatefulsetDesiredPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetDesiredPods,
                                                    descrMetricK8sStatefulsetDesiredPods,
                                                    unitMetricK8sStatefulsetDesiredPods);
}

/**
  The number of replica pods created by the statefulset controller from the statefulset version
  indicated by currentRevision. <p> This metric aligns with the @code currentReplicas @endcode field
  of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetstatus-v1-apps">K8s
  StatefulSetStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sStatefulsetPodCurrent = "k8s.statefulset.pod.current";
static constexpr const char *descrMetricK8sStatefulsetPodCurrent =
    "The number of replica pods created by the statefulset controller from the statefulset version "
    "indicated by currentRevision.";
static constexpr const char *unitMetricK8sStatefulsetPodCurrent = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetPodCurrent(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetPodCurrent,
                                         descrMetricK8sStatefulsetPodCurrent,
                                         unitMetricK8sStatefulsetPodCurrent);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetPodCurrent(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetPodCurrent,
                                          descrMetricK8sStatefulsetPodCurrent,
                                          unitMetricK8sStatefulsetPodCurrent);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetPodCurrent(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetPodCurrent,
                                                   descrMetricK8sStatefulsetPodCurrent,
                                                   unitMetricK8sStatefulsetPodCurrent);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetPodCurrent(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetPodCurrent,
                                                    descrMetricK8sStatefulsetPodCurrent,
                                                    unitMetricK8sStatefulsetPodCurrent);
}

/**
  Number of desired replica pods in this statefulset.
  <p>
  This metric aligns with the @code replicas @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetspec-v1-apps">K8s
  StatefulSetSpec</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sStatefulsetPodDesired = "k8s.statefulset.pod.desired";
static constexpr const char *descrMetricK8sStatefulsetPodDesired =
    "Number of desired replica pods in this statefulset.";
static constexpr const char *unitMetricK8sStatefulsetPodDesired = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetPodDesired(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetPodDesired,
                                         descrMetricK8sStatefulsetPodDesired,
                                         unitMetricK8sStatefulsetPodDesired);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetPodDesired(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetPodDesired,
                                          descrMetricK8sStatefulsetPodDesired,
                                          unitMetricK8sStatefulsetPodDesired);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetPodDesired(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetPodDesired,
                                                   descrMetricK8sStatefulsetPodDesired,
                                                   unitMetricK8sStatefulsetPodDesired);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetPodDesired(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetPodDesired,
                                                    descrMetricK8sStatefulsetPodDesired,
                                                    unitMetricK8sStatefulsetPodDesired);
}

/**
  The number of replica pods created for this statefulset with a Ready Condition.
  <p>
  This metric aligns with the @code readyReplicas @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetstatus-v1-apps">K8s
  StatefulSetStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sStatefulsetPodReady = "k8s.statefulset.pod.ready";
static constexpr const char *descrMetricK8sStatefulsetPodReady =
    "The number of replica pods created for this statefulset with a Ready Condition.";
static constexpr const char *unitMetricK8sStatefulsetPodReady = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetPodReady(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetPodReady,
                                         descrMetricK8sStatefulsetPodReady,
                                         unitMetricK8sStatefulsetPodReady);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetPodReady(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetPodReady,
                                          descrMetricK8sStatefulsetPodReady,
                                          unitMetricK8sStatefulsetPodReady);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetPodReady(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetPodReady,
                                                   descrMetricK8sStatefulsetPodReady,
                                                   unitMetricK8sStatefulsetPodReady);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetPodReady(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetPodReady,
                                                    descrMetricK8sStatefulsetPodReady,
                                                    unitMetricK8sStatefulsetPodReady);
}

/**
  Number of replica pods created by the statefulset controller from the statefulset version
  indicated by updateRevision. <p> This metric aligns with the @code updatedReplicas @endcode field
  of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetstatus-v1-apps">K8s
  StatefulSetStatus</a>. <p> updowncounter
 */
static constexpr const char *kMetricK8sStatefulsetPodUpdated = "k8s.statefulset.pod.updated";
static constexpr const char *descrMetricK8sStatefulsetPodUpdated =
    "Number of replica pods created by the statefulset controller from the statefulset version "
    "indicated by updateRevision.";
static constexpr const char *unitMetricK8sStatefulsetPodUpdated = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetPodUpdated(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetPodUpdated,
                                         descrMetricK8sStatefulsetPodUpdated,
                                         unitMetricK8sStatefulsetPodUpdated);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetPodUpdated(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetPodUpdated,
                                          descrMetricK8sStatefulsetPodUpdated,
                                          unitMetricK8sStatefulsetPodUpdated);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetPodUpdated(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetPodUpdated,
                                                   descrMetricK8sStatefulsetPodUpdated,
                                                   unitMetricK8sStatefulsetPodUpdated);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetPodUpdated(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetPodUpdated,
                                                    descrMetricK8sStatefulsetPodUpdated,
                                                    unitMetricK8sStatefulsetPodUpdated);
}

/**
  Deprecated, use @code k8s.statefulset.pod.ready @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.statefulset.pod.ready @endcode.", "reason": "renamed",
  "renamed_to": "k8s.statefulset.pod.ready"} <p> This metric aligns with the @code readyReplicas
  @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetstatus-v1-apps">K8s
  StatefulSetStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sStatefulsetReadyPods =
    "k8s.statefulset.ready_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sStatefulsetReadyPods =
    "Deprecated, use `k8s.statefulset.pod.ready` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sStatefulsetReadyPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetReadyPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetReadyPods,
                                         descrMetricK8sStatefulsetReadyPods,
                                         unitMetricK8sStatefulsetReadyPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetReadyPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetReadyPods,
                                          descrMetricK8sStatefulsetReadyPods,
                                          unitMetricK8sStatefulsetReadyPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetReadyPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetReadyPods,
                                                   descrMetricK8sStatefulsetReadyPods,
                                                   unitMetricK8sStatefulsetReadyPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetReadyPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetReadyPods,
                                                    descrMetricK8sStatefulsetReadyPods,
                                                    unitMetricK8sStatefulsetReadyPods);
}

/**
  Deprecated, use @code k8s.statefulset.pod.updated @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.statefulset.pod.updated @endcode.", "reason": "renamed",
  "renamed_to": "k8s.statefulset.pod.updated"} <p> This metric aligns with the @code updatedReplicas
  @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetstatus-v1-apps">K8s
  StatefulSetStatus</a>. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sStatefulsetUpdatedPods =
    "k8s.statefulset.updated_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char *descrMetricK8sStatefulsetUpdatedPods =
    "Deprecated, use `k8s.statefulset.pod.updated` instead.";
OPENTELEMETRY_DEPRECATED static constexpr const char *unitMetricK8sStatefulsetUpdatedPods = "{pod}";

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetUpdatedPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetUpdatedPods,
                                         descrMetricK8sStatefulsetUpdatedPods,
                                         unitMetricK8sStatefulsetUpdatedPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetUpdatedPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetUpdatedPods,
                                          descrMetricK8sStatefulsetUpdatedPods,
                                          unitMetricK8sStatefulsetUpdatedPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetUpdatedPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetUpdatedPods,
                                                   descrMetricK8sStatefulsetUpdatedPods,
                                                   unitMetricK8sStatefulsetUpdatedPods);
}

OPENTELEMETRY_DEPRECATED static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetUpdatedPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetUpdatedPods,
                                                    descrMetricK8sStatefulsetUpdatedPods,
                                                    unitMetricK8sStatefulsetUpdatedPods);
}

}  // namespace k8s
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
