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
namespace openshift
{

/**
  The enforced hard limit of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaCpuLimitHard =
    "openshift.clusterquota.cpu.limit.hard";
static constexpr const char *descrMetricOpenshiftClusterquotaCpuLimitHard =
    "The enforced hard limit of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaCpuLimitHard = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaCpuLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaCpuLimitHard,
                                         descrMetricOpenshiftClusterquotaCpuLimitHard,
                                         unitMetricOpenshiftClusterquotaCpuLimitHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaCpuLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaCpuLimitHard,
                                          descrMetricOpenshiftClusterquotaCpuLimitHard,
                                          unitMetricOpenshiftClusterquotaCpuLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaCpuLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOpenshiftClusterquotaCpuLimitHard,
                                                   descrMetricOpenshiftClusterquotaCpuLimitHard,
                                                   unitMetricOpenshiftClusterquotaCpuLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaCpuLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOpenshiftClusterquotaCpuLimitHard,
                                                    descrMetricOpenshiftClusterquotaCpuLimitHard,
                                                    unitMetricOpenshiftClusterquotaCpuLimitHard);
}

/**
  The current observed total usage of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaCpuLimitUsed =
    "openshift.clusterquota.cpu.limit.used";
static constexpr const char *descrMetricOpenshiftClusterquotaCpuLimitUsed =
    "The current observed total usage of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaCpuLimitUsed = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaCpuLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaCpuLimitUsed,
                                         descrMetricOpenshiftClusterquotaCpuLimitUsed,
                                         unitMetricOpenshiftClusterquotaCpuLimitUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaCpuLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaCpuLimitUsed,
                                          descrMetricOpenshiftClusterquotaCpuLimitUsed,
                                          unitMetricOpenshiftClusterquotaCpuLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaCpuLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOpenshiftClusterquotaCpuLimitUsed,
                                                   descrMetricOpenshiftClusterquotaCpuLimitUsed,
                                                   unitMetricOpenshiftClusterquotaCpuLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaCpuLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOpenshiftClusterquotaCpuLimitUsed,
                                                    descrMetricOpenshiftClusterquotaCpuLimitUsed,
                                                    unitMetricOpenshiftClusterquotaCpuLimitUsed);
}

/**
  The enforced hard limit of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaCpuRequestHard =
    "openshift.clusterquota.cpu.request.hard";
static constexpr const char *descrMetricOpenshiftClusterquotaCpuRequestHard =
    "The enforced hard limit of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaCpuRequestHard = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaCpuRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaCpuRequestHard,
                                         descrMetricOpenshiftClusterquotaCpuRequestHard,
                                         unitMetricOpenshiftClusterquotaCpuRequestHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaCpuRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaCpuRequestHard,
                                          descrMetricOpenshiftClusterquotaCpuRequestHard,
                                          unitMetricOpenshiftClusterquotaCpuRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaCpuRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOpenshiftClusterquotaCpuRequestHard,
                                                   descrMetricOpenshiftClusterquotaCpuRequestHard,
                                                   unitMetricOpenshiftClusterquotaCpuRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaCpuRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOpenshiftClusterquotaCpuRequestHard,
                                                    descrMetricOpenshiftClusterquotaCpuRequestHard,
                                                    unitMetricOpenshiftClusterquotaCpuRequestHard);
}

/**
  The current observed total usage of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaCpuRequestUsed =
    "openshift.clusterquota.cpu.request.used";
static constexpr const char *descrMetricOpenshiftClusterquotaCpuRequestUsed =
    "The current observed total usage of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaCpuRequestUsed = "{cpu}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaCpuRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaCpuRequestUsed,
                                         descrMetricOpenshiftClusterquotaCpuRequestUsed,
                                         unitMetricOpenshiftClusterquotaCpuRequestUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaCpuRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaCpuRequestUsed,
                                          descrMetricOpenshiftClusterquotaCpuRequestUsed,
                                          unitMetricOpenshiftClusterquotaCpuRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaCpuRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOpenshiftClusterquotaCpuRequestUsed,
                                                   descrMetricOpenshiftClusterquotaCpuRequestUsed,
                                                   unitMetricOpenshiftClusterquotaCpuRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaCpuRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOpenshiftClusterquotaCpuRequestUsed,
                                                    descrMetricOpenshiftClusterquotaCpuRequestUsed,
                                                    unitMetricOpenshiftClusterquotaCpuRequestUsed);
}

/**
  The enforced hard limit of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaEphemeralStorageLimitHard =
    "openshift.clusterquota.ephemeral_storage.limit.hard";
static constexpr const char *descrMetricOpenshiftClusterquotaEphemeralStorageLimitHard =
    "The enforced hard limit of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaEphemeralStorageLimitHard = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaEphemeralStorageLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaEphemeralStorageLimitHard,
                                         descrMetricOpenshiftClusterquotaEphemeralStorageLimitHard,
                                         unitMetricOpenshiftClusterquotaEphemeralStorageLimitHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaEphemeralStorageLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaEphemeralStorageLimitHard,
                                          descrMetricOpenshiftClusterquotaEphemeralStorageLimitHard,
                                          unitMetricOpenshiftClusterquotaEphemeralStorageLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaEphemeralStorageLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageLimitHard,
      descrMetricOpenshiftClusterquotaEphemeralStorageLimitHard,
      unitMetricOpenshiftClusterquotaEphemeralStorageLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaEphemeralStorageLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageLimitHard,
      descrMetricOpenshiftClusterquotaEphemeralStorageLimitHard,
      unitMetricOpenshiftClusterquotaEphemeralStorageLimitHard);
}

/**
  The current observed total usage of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaEphemeralStorageLimitUsed =
    "openshift.clusterquota.ephemeral_storage.limit.used";
static constexpr const char *descrMetricOpenshiftClusterquotaEphemeralStorageLimitUsed =
    "The current observed total usage of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaEphemeralStorageLimitUsed = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaEphemeralStorageLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaEphemeralStorageLimitUsed,
                                         descrMetricOpenshiftClusterquotaEphemeralStorageLimitUsed,
                                         unitMetricOpenshiftClusterquotaEphemeralStorageLimitUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaEphemeralStorageLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaEphemeralStorageLimitUsed,
                                          descrMetricOpenshiftClusterquotaEphemeralStorageLimitUsed,
                                          unitMetricOpenshiftClusterquotaEphemeralStorageLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaEphemeralStorageLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageLimitUsed,
      descrMetricOpenshiftClusterquotaEphemeralStorageLimitUsed,
      unitMetricOpenshiftClusterquotaEphemeralStorageLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaEphemeralStorageLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageLimitUsed,
      descrMetricOpenshiftClusterquotaEphemeralStorageLimitUsed,
      unitMetricOpenshiftClusterquotaEphemeralStorageLimitUsed);
}

/**
  The enforced hard limit of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaEphemeralStorageRequestHard =
    "openshift.clusterquota.ephemeral_storage.request.hard";
static constexpr const char *descrMetricOpenshiftClusterquotaEphemeralStorageRequestHard =
    "The enforced hard limit of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaEphemeralStorageRequestHard = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaEphemeralStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageRequestHard,
      descrMetricOpenshiftClusterquotaEphemeralStorageRequestHard,
      unitMetricOpenshiftClusterquotaEphemeralStorageRequestHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaEphemeralStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageRequestHard,
      descrMetricOpenshiftClusterquotaEphemeralStorageRequestHard,
      unitMetricOpenshiftClusterquotaEphemeralStorageRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaEphemeralStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageRequestHard,
      descrMetricOpenshiftClusterquotaEphemeralStorageRequestHard,
      unitMetricOpenshiftClusterquotaEphemeralStorageRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaEphemeralStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageRequestHard,
      descrMetricOpenshiftClusterquotaEphemeralStorageRequestHard,
      unitMetricOpenshiftClusterquotaEphemeralStorageRequestHard);
}

/**
  The current observed total usage of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaEphemeralStorageRequestUsed =
    "openshift.clusterquota.ephemeral_storage.request.used";
static constexpr const char *descrMetricOpenshiftClusterquotaEphemeralStorageRequestUsed =
    "The current observed total usage of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaEphemeralStorageRequestUsed = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaEphemeralStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageRequestUsed,
      descrMetricOpenshiftClusterquotaEphemeralStorageRequestUsed,
      unitMetricOpenshiftClusterquotaEphemeralStorageRequestUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaEphemeralStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageRequestUsed,
      descrMetricOpenshiftClusterquotaEphemeralStorageRequestUsed,
      unitMetricOpenshiftClusterquotaEphemeralStorageRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaEphemeralStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageRequestUsed,
      descrMetricOpenshiftClusterquotaEphemeralStorageRequestUsed,
      unitMetricOpenshiftClusterquotaEphemeralStorageRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaEphemeralStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaEphemeralStorageRequestUsed,
      descrMetricOpenshiftClusterquotaEphemeralStorageRequestUsed,
      unitMetricOpenshiftClusterquotaEphemeralStorageRequestUsed);
}

/**
  The enforced hard limit of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaHugepageCountRequestHard =
    "openshift.clusterquota.hugepage_count.request.hard";
static constexpr const char *descrMetricOpenshiftClusterquotaHugepageCountRequestHard =
    "The enforced hard limit of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaHugepageCountRequestHard =
        "{hugepage}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaHugepageCountRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaHugepageCountRequestHard,
                                         descrMetricOpenshiftClusterquotaHugepageCountRequestHard,
                                         unitMetricOpenshiftClusterquotaHugepageCountRequestHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaHugepageCountRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaHugepageCountRequestHard,
                                          descrMetricOpenshiftClusterquotaHugepageCountRequestHard,
                                          unitMetricOpenshiftClusterquotaHugepageCountRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaHugepageCountRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaHugepageCountRequestHard,
      descrMetricOpenshiftClusterquotaHugepageCountRequestHard,
      unitMetricOpenshiftClusterquotaHugepageCountRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaHugepageCountRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaHugepageCountRequestHard,
      descrMetricOpenshiftClusterquotaHugepageCountRequestHard,
      unitMetricOpenshiftClusterquotaHugepageCountRequestHard);
}

/**
  The current observed total usage of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaHugepageCountRequestUsed =
    "openshift.clusterquota.hugepage_count.request.used";
static constexpr const char *descrMetricOpenshiftClusterquotaHugepageCountRequestUsed =
    "The current observed total usage of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaHugepageCountRequestUsed =
        "{hugepage}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaHugepageCountRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaHugepageCountRequestUsed,
                                         descrMetricOpenshiftClusterquotaHugepageCountRequestUsed,
                                         unitMetricOpenshiftClusterquotaHugepageCountRequestUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaHugepageCountRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaHugepageCountRequestUsed,
                                          descrMetricOpenshiftClusterquotaHugepageCountRequestUsed,
                                          unitMetricOpenshiftClusterquotaHugepageCountRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaHugepageCountRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaHugepageCountRequestUsed,
      descrMetricOpenshiftClusterquotaHugepageCountRequestUsed,
      unitMetricOpenshiftClusterquotaHugepageCountRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaHugepageCountRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaHugepageCountRequestUsed,
      descrMetricOpenshiftClusterquotaHugepageCountRequestUsed,
      unitMetricOpenshiftClusterquotaHugepageCountRequestUsed);
}

/**
  The enforced hard limit of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaMemoryLimitHard =
    "openshift.clusterquota.memory.limit.hard";
static constexpr const char *descrMetricOpenshiftClusterquotaMemoryLimitHard =
    "The enforced hard limit of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaMemoryLimitHard = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaMemoryLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaMemoryLimitHard,
                                         descrMetricOpenshiftClusterquotaMemoryLimitHard,
                                         unitMetricOpenshiftClusterquotaMemoryLimitHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaMemoryLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaMemoryLimitHard,
                                          descrMetricOpenshiftClusterquotaMemoryLimitHard,
                                          unitMetricOpenshiftClusterquotaMemoryLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaMemoryLimitHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOpenshiftClusterquotaMemoryLimitHard,
                                                   descrMetricOpenshiftClusterquotaMemoryLimitHard,
                                                   unitMetricOpenshiftClusterquotaMemoryLimitHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaMemoryLimitHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOpenshiftClusterquotaMemoryLimitHard,
                                                    descrMetricOpenshiftClusterquotaMemoryLimitHard,
                                                    unitMetricOpenshiftClusterquotaMemoryLimitHard);
}

/**
  The current observed total usage of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaMemoryLimitUsed =
    "openshift.clusterquota.memory.limit.used";
static constexpr const char *descrMetricOpenshiftClusterquotaMemoryLimitUsed =
    "The current observed total usage of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaMemoryLimitUsed = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaMemoryLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaMemoryLimitUsed,
                                         descrMetricOpenshiftClusterquotaMemoryLimitUsed,
                                         unitMetricOpenshiftClusterquotaMemoryLimitUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaMemoryLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaMemoryLimitUsed,
                                          descrMetricOpenshiftClusterquotaMemoryLimitUsed,
                                          unitMetricOpenshiftClusterquotaMemoryLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaMemoryLimitUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOpenshiftClusterquotaMemoryLimitUsed,
                                                   descrMetricOpenshiftClusterquotaMemoryLimitUsed,
                                                   unitMetricOpenshiftClusterquotaMemoryLimitUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaMemoryLimitUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOpenshiftClusterquotaMemoryLimitUsed,
                                                    descrMetricOpenshiftClusterquotaMemoryLimitUsed,
                                                    unitMetricOpenshiftClusterquotaMemoryLimitUsed);
}

/**
  The enforced hard limit of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaMemoryRequestHard =
    "openshift.clusterquota.memory.request.hard";
static constexpr const char *descrMetricOpenshiftClusterquotaMemoryRequestHard =
    "The enforced hard limit of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaMemoryRequestHard = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaMemoryRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaMemoryRequestHard,
                                         descrMetricOpenshiftClusterquotaMemoryRequestHard,
                                         unitMetricOpenshiftClusterquotaMemoryRequestHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaMemoryRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaMemoryRequestHard,
                                          descrMetricOpenshiftClusterquotaMemoryRequestHard,
                                          unitMetricOpenshiftClusterquotaMemoryRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaMemoryRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaMemoryRequestHard,
      descrMetricOpenshiftClusterquotaMemoryRequestHard,
      unitMetricOpenshiftClusterquotaMemoryRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaMemoryRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaMemoryRequestHard,
      descrMetricOpenshiftClusterquotaMemoryRequestHard,
      unitMetricOpenshiftClusterquotaMemoryRequestHard);
}

/**
  The current observed total usage of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaMemoryRequestUsed =
    "openshift.clusterquota.memory.request.used";
static constexpr const char *descrMetricOpenshiftClusterquotaMemoryRequestUsed =
    "The current observed total usage of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaMemoryRequestUsed = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaMemoryRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaMemoryRequestUsed,
                                         descrMetricOpenshiftClusterquotaMemoryRequestUsed,
                                         unitMetricOpenshiftClusterquotaMemoryRequestUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaMemoryRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaMemoryRequestUsed,
                                          descrMetricOpenshiftClusterquotaMemoryRequestUsed,
                                          unitMetricOpenshiftClusterquotaMemoryRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaMemoryRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaMemoryRequestUsed,
      descrMetricOpenshiftClusterquotaMemoryRequestUsed,
      unitMetricOpenshiftClusterquotaMemoryRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaMemoryRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaMemoryRequestUsed,
      descrMetricOpenshiftClusterquotaMemoryRequestUsed,
      unitMetricOpenshiftClusterquotaMemoryRequestUsed);
}

/**
  The enforced hard limit of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaObjectCountHard =
    "openshift.clusterquota.object_count.hard";
static constexpr const char *descrMetricOpenshiftClusterquotaObjectCountHard =
    "The enforced hard limit of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaObjectCountHard = "{object}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaObjectCountHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaObjectCountHard,
                                         descrMetricOpenshiftClusterquotaObjectCountHard,
                                         unitMetricOpenshiftClusterquotaObjectCountHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaObjectCountHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaObjectCountHard,
                                          descrMetricOpenshiftClusterquotaObjectCountHard,
                                          unitMetricOpenshiftClusterquotaObjectCountHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaObjectCountHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOpenshiftClusterquotaObjectCountHard,
                                                   descrMetricOpenshiftClusterquotaObjectCountHard,
                                                   unitMetricOpenshiftClusterquotaObjectCountHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaObjectCountHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOpenshiftClusterquotaObjectCountHard,
                                                    descrMetricOpenshiftClusterquotaObjectCountHard,
                                                    unitMetricOpenshiftClusterquotaObjectCountHard);
}

/**
  The current observed total usage of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaObjectCountUsed =
    "openshift.clusterquota.object_count.used";
static constexpr const char *descrMetricOpenshiftClusterquotaObjectCountUsed =
    "The current observed total usage of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaObjectCountUsed = "{object}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaObjectCountUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaObjectCountUsed,
                                         descrMetricOpenshiftClusterquotaObjectCountUsed,
                                         unitMetricOpenshiftClusterquotaObjectCountUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaObjectCountUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaObjectCountUsed,
                                          descrMetricOpenshiftClusterquotaObjectCountUsed,
                                          unitMetricOpenshiftClusterquotaObjectCountUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaObjectCountUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricOpenshiftClusterquotaObjectCountUsed,
                                                   descrMetricOpenshiftClusterquotaObjectCountUsed,
                                                   unitMetricOpenshiftClusterquotaObjectCountUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaObjectCountUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricOpenshiftClusterquotaObjectCountUsed,
                                                    descrMetricOpenshiftClusterquotaObjectCountUsed,
                                                    unitMetricOpenshiftClusterquotaObjectCountUsed);
}

/**
  The enforced hard limit of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  The @code k8s.storageclass.name @endcode should be required when a resource quota is defined for a
  specific storage class. <p> updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard =
    "openshift.clusterquota.persistentvolumeclaim_count.hard";
static constexpr const char *descrMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard =
    "The enforced hard limit of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard =
        "{persistentvolumeclaim}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaPersistentvolumeclaimCountHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard,
      descrMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard,
      unitMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard,
      descrMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard,
      unitMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaPersistentvolumeclaimCountHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard,
      descrMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard,
      unitMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard,
      descrMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard,
      unitMetricOpenshiftClusterquotaPersistentvolumeclaimCountHard);
}

/**
  The current observed total usage of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  The @code k8s.storageclass.name @endcode should be required when a resource quota is defined for a
  specific storage class. <p> updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed =
    "openshift.clusterquota.persistentvolumeclaim_count.used";
static constexpr const char *descrMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed =
    "The current observed total usage of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed =
        "{persistentvolumeclaim}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed,
      descrMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed,
      unitMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed,
      descrMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed,
      unitMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed,
      descrMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed,
      unitMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed,
      descrMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed,
      unitMetricOpenshiftClusterquotaPersistentvolumeclaimCountUsed);
}

/**
  The enforced hard limit of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Hard @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  The @code k8s.storageclass.name @endcode should be required when a resource quota is defined for a
  specific storage class. <p> updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaStorageRequestHard =
    "openshift.clusterquota.storage.request.hard";
static constexpr const char *descrMetricOpenshiftClusterquotaStorageRequestHard =
    "The enforced hard limit of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaStorageRequestHard = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaStorageRequestHard,
                                         descrMetricOpenshiftClusterquotaStorageRequestHard,
                                         unitMetricOpenshiftClusterquotaStorageRequestHard);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaStorageRequestHard,
                                          descrMetricOpenshiftClusterquotaStorageRequestHard,
                                          unitMetricOpenshiftClusterquotaStorageRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaStorageRequestHard,
      descrMetricOpenshiftClusterquotaStorageRequestHard,
      unitMetricOpenshiftClusterquotaStorageRequestHard);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaStorageRequestHard(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaStorageRequestHard,
      descrMetricOpenshiftClusterquotaStorageRequestHard,
      unitMetricOpenshiftClusterquotaStorageRequestHard);
}

/**
  The current observed total usage of the resource across all projects.
  <p>
  This metric is retrieved from the @code Status.Total.Used @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.32/#resourcequotastatus-v1-core">K8s
  ResourceQuotaStatus</a> of the <a
  href="https://docs.redhat.com/en/documentation/openshift_container_platform/4.19/html/schedule_and_quota_apis/clusterresourcequota-quota-openshift-io-v1#status-total">ClusterResourceQuota</a>.
  <p>
  The @code k8s.storageclass.name @endcode should be required when a resource quota is defined for a
  specific storage class. <p> updowncounter
 */
static constexpr const char *kMetricOpenshiftClusterquotaStorageRequestUsed =
    "openshift.clusterquota.storage.request.used";
static constexpr const char *descrMetricOpenshiftClusterquotaStorageRequestUsed =
    "The current observed total usage of the resource across all projects.
    ";
    static constexpr const char *unitMetricOpenshiftClusterquotaStorageRequestUsed = "By";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricOpenshiftClusterquotaStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricOpenshiftClusterquotaStorageRequestUsed,
                                         descrMetricOpenshiftClusterquotaStorageRequestUsed,
                                         unitMetricOpenshiftClusterquotaStorageRequestUsed);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricOpenshiftClusterquotaStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricOpenshiftClusterquotaStorageRequestUsed,
                                          descrMetricOpenshiftClusterquotaStorageRequestUsed,
                                          unitMetricOpenshiftClusterquotaStorageRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricOpenshiftClusterquotaStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricOpenshiftClusterquotaStorageRequestUsed,
      descrMetricOpenshiftClusterquotaStorageRequestUsed,
      unitMetricOpenshiftClusterquotaStorageRequestUsed);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricOpenshiftClusterquotaStorageRequestUsed(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricOpenshiftClusterquotaStorageRequestUsed,
      descrMetricOpenshiftClusterquotaStorageRequestUsed,
      unitMetricOpenshiftClusterquotaStorageRequestUsed);
}

}  // namespace openshift
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
