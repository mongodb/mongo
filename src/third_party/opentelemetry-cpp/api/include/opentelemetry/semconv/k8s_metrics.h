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
 * The number of actively running jobs for a cronjob
 * <p>
 * This metric aligns with the @code active @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#cronjobstatus-v1-batch">K8s
 * CronJobStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#cronjob">@code k8s.cronjob @endcode</a> resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sCronjobActiveJobs = "k8s.cronjob.active_jobs";
static constexpr const char *descrMetricK8sCronjobActiveJobs =
    "The number of actively running jobs for a cronjob";
static constexpr const char *unitMetricK8sCronjobActiveJobs = "{job}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sCronjobActiveJobs(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricK8sCronjobActiveJobs, descrMetricK8sCronjobActiveJobs, unitMetricK8sCronjobActiveJobs);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sCronjobActiveJobs(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricK8sCronjobActiveJobs, descrMetricK8sCronjobActiveJobs, unitMetricK8sCronjobActiveJobs);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sCronjobActiveJobs(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sCronjobActiveJobs, descrMetricK8sCronjobActiveJobs, unitMetricK8sCronjobActiveJobs);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sCronjobActiveJobs(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sCronjobActiveJobs, descrMetricK8sCronjobActiveJobs, unitMetricK8sCronjobActiveJobs);
}

/**
 * Number of nodes that are running at least 1 daemon pod and are supposed to run the daemon pod
 * <p>
 * This metric aligns with the @code currentNumberScheduled @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
 * DaemonSetStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#daemonset">@code k8s.daemonset @endcode</a> resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sDaemonsetCurrentScheduledNodes =
    "k8s.daemonset.current_scheduled_nodes";
static constexpr const char *descrMetricK8sDaemonsetCurrentScheduledNodes =
    "Number of nodes that are running at least 1 daemon pod and are supposed to run the daemon pod";
static constexpr const char *unitMetricK8sDaemonsetCurrentScheduledNodes = "{node}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetCurrentScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetCurrentScheduledNodes,
                                         descrMetricK8sDaemonsetCurrentScheduledNodes,
                                         unitMetricK8sDaemonsetCurrentScheduledNodes);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetCurrentScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetCurrentScheduledNodes,
                                          descrMetricK8sDaemonsetCurrentScheduledNodes,
                                          unitMetricK8sDaemonsetCurrentScheduledNodes);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetCurrentScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetCurrentScheduledNodes,
                                                   descrMetricK8sDaemonsetCurrentScheduledNodes,
                                                   unitMetricK8sDaemonsetCurrentScheduledNodes);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetCurrentScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetCurrentScheduledNodes,
                                                    descrMetricK8sDaemonsetCurrentScheduledNodes,
                                                    unitMetricK8sDaemonsetCurrentScheduledNodes);
}

/**
 * Number of nodes that should be running the daemon pod (including nodes currently running the
 * daemon pod) <p> This metric aligns with the @code desiredNumberScheduled @endcode field of the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
 * DaemonSetStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#daemonset">@code k8s.daemonset @endcode</a> resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sDaemonsetDesiredScheduledNodes =
    "k8s.daemonset.desired_scheduled_nodes";
static constexpr const char *descrMetricK8sDaemonsetDesiredScheduledNodes =
    "Number of nodes that should be running the daemon pod (including nodes currently running the "
    "daemon pod)";
static constexpr const char *unitMetricK8sDaemonsetDesiredScheduledNodes = "{node}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetDesiredScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetDesiredScheduledNodes,
                                         descrMetricK8sDaemonsetDesiredScheduledNodes,
                                         unitMetricK8sDaemonsetDesiredScheduledNodes);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetDesiredScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetDesiredScheduledNodes,
                                          descrMetricK8sDaemonsetDesiredScheduledNodes,
                                          unitMetricK8sDaemonsetDesiredScheduledNodes);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetDesiredScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetDesiredScheduledNodes,
                                                   descrMetricK8sDaemonsetDesiredScheduledNodes,
                                                   unitMetricK8sDaemonsetDesiredScheduledNodes);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetDesiredScheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetDesiredScheduledNodes,
                                                    descrMetricK8sDaemonsetDesiredScheduledNodes,
                                                    unitMetricK8sDaemonsetDesiredScheduledNodes);
}

/**
 * Number of nodes that are running the daemon pod, but are not supposed to run the daemon pod
 * <p>
 * This metric aligns with the @code numberMisscheduled @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
 * DaemonSetStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#daemonset">@code k8s.daemonset @endcode</a> resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sDaemonsetMisscheduledNodes =
    "k8s.daemonset.misscheduled_nodes";
static constexpr const char *descrMetricK8sDaemonsetMisscheduledNodes =
    "Number of nodes that are running the daemon pod, but are not supposed to run the daemon pod";
static constexpr const char *unitMetricK8sDaemonsetMisscheduledNodes = "{node}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetMisscheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetMisscheduledNodes,
                                         descrMetricK8sDaemonsetMisscheduledNodes,
                                         unitMetricK8sDaemonsetMisscheduledNodes);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetMisscheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetMisscheduledNodes,
                                          descrMetricK8sDaemonsetMisscheduledNodes,
                                          unitMetricK8sDaemonsetMisscheduledNodes);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetMisscheduledNodes(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetMisscheduledNodes,
                                                   descrMetricK8sDaemonsetMisscheduledNodes,
                                                   unitMetricK8sDaemonsetMisscheduledNodes);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetMisscheduledNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetMisscheduledNodes,
                                                    descrMetricK8sDaemonsetMisscheduledNodes,
                                                    unitMetricK8sDaemonsetMisscheduledNodes);
}

/**
 * Number of nodes that should be running the daemon pod and have one or more of the daemon pod
 * running and ready <p> This metric aligns with the @code numberReady @endcode field of the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#daemonsetstatus-v1-apps">K8s
 * DaemonSetStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#daemonset">@code k8s.daemonset @endcode</a> resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sDaemonsetReadyNodes = "k8s.daemonset.ready_nodes";
static constexpr const char *descrMetricK8sDaemonsetReadyNodes =
    "Number of nodes that should be running the daemon pod and have one or more of the daemon pod "
    "running and ready";
static constexpr const char *unitMetricK8sDaemonsetReadyNodes = "{node}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDaemonsetReadyNodes(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDaemonsetReadyNodes,
                                         descrMetricK8sDaemonsetReadyNodes,
                                         unitMetricK8sDaemonsetReadyNodes);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDaemonsetReadyNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDaemonsetReadyNodes,
                                          descrMetricK8sDaemonsetReadyNodes,
                                          unitMetricK8sDaemonsetReadyNodes);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDaemonsetReadyNodes(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDaemonsetReadyNodes,
                                                   descrMetricK8sDaemonsetReadyNodes,
                                                   unitMetricK8sDaemonsetReadyNodes);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDaemonsetReadyNodes(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDaemonsetReadyNodes,
                                                    descrMetricK8sDaemonsetReadyNodes,
                                                    unitMetricK8sDaemonsetReadyNodes);
}

/**
 * Total number of available replica pods (ready for at least minReadySeconds) targeted by this
 * deployment <p> This metric aligns with the @code availableReplicas @endcode field of the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#deploymentstatus-v1-apps">K8s
 * DeploymentStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#deployment">@code k8s.deployment @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sDeploymentAvailablePods = "k8s.deployment.available_pods";
static constexpr const char *descrMetricK8sDeploymentAvailablePods =
    "Total number of available replica pods (ready for at least minReadySeconds) targeted by this "
    "deployment";
static constexpr const char *unitMetricK8sDeploymentAvailablePods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDeploymentAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDeploymentAvailablePods,
                                         descrMetricK8sDeploymentAvailablePods,
                                         unitMetricK8sDeploymentAvailablePods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDeploymentAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDeploymentAvailablePods,
                                          descrMetricK8sDeploymentAvailablePods,
                                          unitMetricK8sDeploymentAvailablePods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDeploymentAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDeploymentAvailablePods,
                                                   descrMetricK8sDeploymentAvailablePods,
                                                   unitMetricK8sDeploymentAvailablePods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDeploymentAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDeploymentAvailablePods,
                                                    descrMetricK8sDeploymentAvailablePods,
                                                    unitMetricK8sDeploymentAvailablePods);
}

/**
 * Number of desired replica pods in this deployment
 * <p>
 * This metric aligns with the @code replicas @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#deploymentspec-v1-apps">K8s
 * DeploymentSpec</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#deployment">@code k8s.deployment @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sDeploymentDesiredPods = "k8s.deployment.desired_pods";
static constexpr const char *descrMetricK8sDeploymentDesiredPods =
    "Number of desired replica pods in this deployment";
static constexpr const char *unitMetricK8sDeploymentDesiredPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sDeploymentDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sDeploymentDesiredPods,
                                         descrMetricK8sDeploymentDesiredPods,
                                         unitMetricK8sDeploymentDesiredPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sDeploymentDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sDeploymentDesiredPods,
                                          descrMetricK8sDeploymentDesiredPods,
                                          unitMetricK8sDeploymentDesiredPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sDeploymentDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sDeploymentDesiredPods,
                                                   descrMetricK8sDeploymentDesiredPods,
                                                   unitMetricK8sDeploymentDesiredPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sDeploymentDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sDeploymentDesiredPods,
                                                    descrMetricK8sDeploymentDesiredPods,
                                                    unitMetricK8sDeploymentDesiredPods);
}

/**
 * Current number of replica pods managed by this horizontal pod autoscaler, as last seen by the
 * autoscaler <p> This metric aligns with the @code currentReplicas @endcode field of the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerstatus-v2-autoscaling">K8s
 * HorizontalPodAutoscalerStatus</a> <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#horizontalpodautoscaler">@code k8s.hpa @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sHpaCurrentPods = "k8s.hpa.current_pods";
static constexpr const char *descrMetricK8sHpaCurrentPods =
    "Current number of replica pods managed by this horizontal pod autoscaler, as last seen by the "
    "autoscaler";
static constexpr const char *unitMetricK8sHpaCurrentPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sHpaCurrentPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaCurrentPods, descrMetricK8sHpaCurrentPods,
                                         unitMetricK8sHpaCurrentPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sHpaCurrentPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaCurrentPods, descrMetricK8sHpaCurrentPods,
                                          unitMetricK8sHpaCurrentPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaCurrentPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sHpaCurrentPods, descrMetricK8sHpaCurrentPods, unitMetricK8sHpaCurrentPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaCurrentPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sHpaCurrentPods, descrMetricK8sHpaCurrentPods, unitMetricK8sHpaCurrentPods);
}

/**
 * Desired number of replica pods managed by this horizontal pod autoscaler, as last calculated by
 * the autoscaler <p> This metric aligns with the @code desiredReplicas @endcode field of the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerstatus-v2-autoscaling">K8s
 * HorizontalPodAutoscalerStatus</a> <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#horizontalpodautoscaler">@code k8s.hpa @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sHpaDesiredPods = "k8s.hpa.desired_pods";
static constexpr const char *descrMetricK8sHpaDesiredPods =
    "Desired number of replica pods managed by this horizontal pod autoscaler, as last calculated "
    "by the autoscaler";
static constexpr const char *unitMetricK8sHpaDesiredPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sHpaDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaDesiredPods, descrMetricK8sHpaDesiredPods,
                                         unitMetricK8sHpaDesiredPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sHpaDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaDesiredPods, descrMetricK8sHpaDesiredPods,
                                          unitMetricK8sHpaDesiredPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sHpaDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sHpaDesiredPods, descrMetricK8sHpaDesiredPods, unitMetricK8sHpaDesiredPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sHpaDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sHpaDesiredPods, descrMetricK8sHpaDesiredPods, unitMetricK8sHpaDesiredPods);
}

/**
 * The upper limit for the number of replica pods to which the autoscaler can scale up
 * <p>
 * This metric aligns with the @code maxReplicas @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerspec-v2-autoscaling">K8s
 * HorizontalPodAutoscalerSpec</a> <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#horizontalpodautoscaler">@code k8s.hpa @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sHpaMaxPods = "k8s.hpa.max_pods";
static constexpr const char *descrMetricK8sHpaMaxPods =
    "The upper limit for the number of replica pods to which the autoscaler can scale up";
static constexpr const char *unitMetricK8sHpaMaxPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>> CreateSyncInt64MetricK8sHpaMaxPods(
    metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaMaxPods, descrMetricK8sHpaMaxPods,
                                         unitMetricK8sHpaMaxPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>> CreateSyncDoubleMetricK8sHpaMaxPods(
    metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaMaxPods, descrMetricK8sHpaMaxPods,
                                          unitMetricK8sHpaMaxPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricK8sHpaMaxPods(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sHpaMaxPods, descrMetricK8sHpaMaxPods,
                                                   unitMetricK8sHpaMaxPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricK8sHpaMaxPods(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sHpaMaxPods, descrMetricK8sHpaMaxPods,
                                                    unitMetricK8sHpaMaxPods);
}

/**
 * The lower limit for the number of replica pods to which the autoscaler can scale down
 * <p>
 * This metric aligns with the @code minReplicas @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#horizontalpodautoscalerspec-v2-autoscaling">K8s
 * HorizontalPodAutoscalerSpec</a> <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#horizontalpodautoscaler">@code k8s.hpa @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sHpaMinPods = "k8s.hpa.min_pods";
static constexpr const char *descrMetricK8sHpaMinPods =
    "The lower limit for the number of replica pods to which the autoscaler can scale down";
static constexpr const char *unitMetricK8sHpaMinPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>> CreateSyncInt64MetricK8sHpaMinPods(
    metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sHpaMinPods, descrMetricK8sHpaMinPods,
                                         unitMetricK8sHpaMinPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>> CreateSyncDoubleMetricK8sHpaMinPods(
    metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sHpaMinPods, descrMetricK8sHpaMinPods,
                                          unitMetricK8sHpaMinPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricK8sHpaMinPods(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sHpaMinPods, descrMetricK8sHpaMinPods,
                                                   unitMetricK8sHpaMinPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricK8sHpaMinPods(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sHpaMinPods, descrMetricK8sHpaMinPods,
                                                    unitMetricK8sHpaMinPods);
}

/**
 * The number of pending and actively running pods for a job
 * <p>
 * This metric aligns with the @code active @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobstatus-v1-batch">K8s
 * JobStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#job">@code k8s.job @endcode</a> resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sJobActivePods = "k8s.job.active_pods";
static constexpr const char *descrMetricK8sJobActivePods =
    "The number of pending and actively running pods for a job";
static constexpr const char *unitMetricK8sJobActivePods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobActivePods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobActivePods, descrMetricK8sJobActivePods,
                                         unitMetricK8sJobActivePods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobActivePods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobActivePods, descrMetricK8sJobActivePods,
                                          unitMetricK8sJobActivePods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobActivePods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sJobActivePods, descrMetricK8sJobActivePods, unitMetricK8sJobActivePods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobActivePods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sJobActivePods, descrMetricK8sJobActivePods, unitMetricK8sJobActivePods);
}

/**
 * The desired number of successfully finished pods the job should be run with
 * <p>
 * This metric aligns with the @code completions @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobspec-v1-batch">K8s
 * JobSpec</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#job">@code k8s.job @endcode</a> resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sJobDesiredSuccessfulPods = "k8s.job.desired_successful_pods";
static constexpr const char *descrMetricK8sJobDesiredSuccessfulPods =
    "The desired number of successfully finished pods the job should be run with";
static constexpr const char *unitMetricK8sJobDesiredSuccessfulPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobDesiredSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobDesiredSuccessfulPods,
                                         descrMetricK8sJobDesiredSuccessfulPods,
                                         unitMetricK8sJobDesiredSuccessfulPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobDesiredSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobDesiredSuccessfulPods,
                                          descrMetricK8sJobDesiredSuccessfulPods,
                                          unitMetricK8sJobDesiredSuccessfulPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobDesiredSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sJobDesiredSuccessfulPods,
                                                   descrMetricK8sJobDesiredSuccessfulPods,
                                                   unitMetricK8sJobDesiredSuccessfulPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobDesiredSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sJobDesiredSuccessfulPods,
                                                    descrMetricK8sJobDesiredSuccessfulPods,
                                                    unitMetricK8sJobDesiredSuccessfulPods);
}

/**
 * The number of pods which reached phase Failed for a job
 * <p>
 * This metric aligns with the @code failed @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobstatus-v1-batch">K8s
 * JobStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#job">@code k8s.job @endcode</a> resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sJobFailedPods = "k8s.job.failed_pods";
static constexpr const char *descrMetricK8sJobFailedPods =
    "The number of pods which reached phase Failed for a job";
static constexpr const char *unitMetricK8sJobFailedPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobFailedPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobFailedPods, descrMetricK8sJobFailedPods,
                                         unitMetricK8sJobFailedPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobFailedPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobFailedPods, descrMetricK8sJobFailedPods,
                                          unitMetricK8sJobFailedPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobFailedPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sJobFailedPods, descrMetricK8sJobFailedPods, unitMetricK8sJobFailedPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobFailedPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sJobFailedPods, descrMetricK8sJobFailedPods, unitMetricK8sJobFailedPods);
}

/**
 * The max desired number of pods the job should run at any given time
 * <p>
 * This metric aligns with the @code parallelism @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobspec-v1-batch">K8s
 * JobSpec</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#job">@code k8s.job @endcode</a> resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sJobMaxParallelPods = "k8s.job.max_parallel_pods";
static constexpr const char *descrMetricK8sJobMaxParallelPods =
    "The max desired number of pods the job should run at any given time";
static constexpr const char *unitMetricK8sJobMaxParallelPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobMaxParallelPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sJobMaxParallelPods,
                                         descrMetricK8sJobMaxParallelPods,
                                         unitMetricK8sJobMaxParallelPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobMaxParallelPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sJobMaxParallelPods,
                                          descrMetricK8sJobMaxParallelPods,
                                          unitMetricK8sJobMaxParallelPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobMaxParallelPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sJobMaxParallelPods,
                                                   descrMetricK8sJobMaxParallelPods,
                                                   unitMetricK8sJobMaxParallelPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobMaxParallelPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sJobMaxParallelPods,
                                                    descrMetricK8sJobMaxParallelPods,
                                                    unitMetricK8sJobMaxParallelPods);
}

/**
 * The number of pods which reached phase Succeeded for a job
 * <p>
 * This metric aligns with the @code succeeded @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#jobstatus-v1-batch">K8s
 * JobStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#job">@code k8s.job @endcode</a> resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sJobSuccessfulPods = "k8s.job.successful_pods";
static constexpr const char *descrMetricK8sJobSuccessfulPods =
    "The number of pods which reached phase Succeeded for a job";
static constexpr const char *unitMetricK8sJobSuccessfulPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sJobSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricK8sJobSuccessfulPods, descrMetricK8sJobSuccessfulPods, unitMetricK8sJobSuccessfulPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sJobSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricK8sJobSuccessfulPods, descrMetricK8sJobSuccessfulPods, unitMetricK8sJobSuccessfulPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sJobSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricK8sJobSuccessfulPods, descrMetricK8sJobSuccessfulPods, unitMetricK8sJobSuccessfulPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sJobSuccessfulPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sJobSuccessfulPods, descrMetricK8sJobSuccessfulPods, unitMetricK8sJobSuccessfulPods);
}

/**
 * Describes number of K8s namespaces that are currently in a given phase.
 * <p>
 * This metric SHOULD, at a minimum, be reported against a
 * <a href="../resource/k8s.md#namespace">@code k8s.namespace @endcode</a> resource.
 * <p>
 * updowncounter
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
 * Total CPU time consumed
 * <p>
 * Total CPU time consumed by the specific Node on all available CPU cores
 * <p>
 * counter
 */
static constexpr const char *kMetricK8sNodeCpuTime     = "k8s.node.cpu.time";
static constexpr const char *descrMetricK8sNodeCpuTime = "Total CPU time consumed";
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
 * Node's CPU usage, measured in cpus. Range from 0 to the number of allocatable CPUs
 * <p>
 * CPU usage of the specific Node on all available CPU cores, averaged over the sample window
 * <p>
 * gauge
 */
static constexpr const char *kMetricK8sNodeCpuUsage = "k8s.node.cpu.usage";
static constexpr const char *descrMetricK8sNodeCpuUsage =
    "Node's CPU usage, measured in cpus. Range from 0 to the number of allocatable CPUs";
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
 * Memory usage of the Node
 * <p>
 * Total memory usage of the Node
 * <p>
 * gauge
 */
static constexpr const char *kMetricK8sNodeMemoryUsage     = "k8s.node.memory.usage";
static constexpr const char *descrMetricK8sNodeMemoryUsage = "Memory usage of the Node";
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
 * Node network errors
 * <p>
 * counter
 */
static constexpr const char *kMetricK8sNodeNetworkErrors     = "k8s.node.network.errors";
static constexpr const char *descrMetricK8sNodeNetworkErrors = "Node network errors";
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
 * Network bytes for the Node
 * <p>
 * counter
 */
static constexpr const char *kMetricK8sNodeNetworkIo     = "k8s.node.network.io";
static constexpr const char *descrMetricK8sNodeNetworkIo = "Network bytes for the Node";
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
 * The time the Node has been running
 * <p>
 * Instrumentations SHOULD use a gauge with type @code double @endcode and measure uptime in seconds
 * as a floating point number with the highest precision available. The actual accuracy would depend
 * on the instrumentation and operating system. <p> gauge
 */
static constexpr const char *kMetricK8sNodeUptime     = "k8s.node.uptime";
static constexpr const char *descrMetricK8sNodeUptime = "The time the Node has been running";
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
 * Total CPU time consumed
 * <p>
 * Total CPU time consumed by the specific Pod on all available CPU cores
 * <p>
 * counter
 */
static constexpr const char *kMetricK8sPodCpuTime     = "k8s.pod.cpu.time";
static constexpr const char *descrMetricK8sPodCpuTime = "Total CPU time consumed";
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
 * Pod's CPU usage, measured in cpus. Range from 0 to the number of allocatable CPUs
 * <p>
 * CPU usage of the specific Pod on all available CPU cores, averaged over the sample window
 * <p>
 * gauge
 */
static constexpr const char *kMetricK8sPodCpuUsage = "k8s.pod.cpu.usage";
static constexpr const char *descrMetricK8sPodCpuUsage =
    "Pod's CPU usage, measured in cpus. Range from 0 to the number of allocatable CPUs";
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
 * Memory usage of the Pod
 * <p>
 * Total memory usage of the Pod
 * <p>
 * gauge
 */
static constexpr const char *kMetricK8sPodMemoryUsage     = "k8s.pod.memory.usage";
static constexpr const char *descrMetricK8sPodMemoryUsage = "Memory usage of the Pod";
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
 * Pod network errors
 * <p>
 * counter
 */
static constexpr const char *kMetricK8sPodNetworkErrors     = "k8s.pod.network.errors";
static constexpr const char *descrMetricK8sPodNetworkErrors = "Pod network errors";
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
 * Network bytes for the Pod
 * <p>
 * counter
 */
static constexpr const char *kMetricK8sPodNetworkIo     = "k8s.pod.network.io";
static constexpr const char *descrMetricK8sPodNetworkIo = "Network bytes for the Pod";
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
 * The time the Pod has been running
 * <p>
 * Instrumentations SHOULD use a gauge with type @code double @endcode and measure uptime in seconds
 * as a floating point number with the highest precision available. The actual accuracy would depend
 * on the instrumentation and operating system. <p> gauge
 */
static constexpr const char *kMetricK8sPodUptime     = "k8s.pod.uptime";
static constexpr const char *descrMetricK8sPodUptime = "The time the Pod has been running";
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
 * Total number of available replica pods (ready for at least minReadySeconds) targeted by this
 * replicaset <p> This metric aligns with the @code availableReplicas @endcode field of the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicasetstatus-v1-apps">K8s
 * ReplicaSetStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#replicaset">@code k8s.replicaset @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sReplicasetAvailablePods = "k8s.replicaset.available_pods";
static constexpr const char *descrMetricK8sReplicasetAvailablePods =
    "Total number of available replica pods (ready for at least minReadySeconds) targeted by this "
    "replicaset";
static constexpr const char *unitMetricK8sReplicasetAvailablePods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicasetAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicasetAvailablePods,
                                         descrMetricK8sReplicasetAvailablePods,
                                         unitMetricK8sReplicasetAvailablePods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicasetAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicasetAvailablePods,
                                          descrMetricK8sReplicasetAvailablePods,
                                          unitMetricK8sReplicasetAvailablePods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicasetAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicasetAvailablePods,
                                                   descrMetricK8sReplicasetAvailablePods,
                                                   unitMetricK8sReplicasetAvailablePods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicasetAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicasetAvailablePods,
                                                    descrMetricK8sReplicasetAvailablePods,
                                                    unitMetricK8sReplicasetAvailablePods);
}

/**
 * Number of desired replica pods in this replicaset
 * <p>
 * This metric aligns with the @code replicas @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicasetspec-v1-apps">K8s
 * ReplicaSetSpec</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#replicaset">@code k8s.replicaset @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sReplicasetDesiredPods = "k8s.replicaset.desired_pods";
static constexpr const char *descrMetricK8sReplicasetDesiredPods =
    "Number of desired replica pods in this replicaset";
static constexpr const char *unitMetricK8sReplicasetDesiredPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicasetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicasetDesiredPods,
                                         descrMetricK8sReplicasetDesiredPods,
                                         unitMetricK8sReplicasetDesiredPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicasetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicasetDesiredPods,
                                          descrMetricK8sReplicasetDesiredPods,
                                          unitMetricK8sReplicasetDesiredPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicasetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicasetDesiredPods,
                                                   descrMetricK8sReplicasetDesiredPods,
                                                   unitMetricK8sReplicasetDesiredPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicasetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicasetDesiredPods,
                                                    descrMetricK8sReplicasetDesiredPods,
                                                    unitMetricK8sReplicasetDesiredPods);
}

/**
 * Deprecated, use @code k8s.replicationcontroller.available_pods @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code k8s.replicationcontroller.available_pods @endcode.", "reason":
 * "uncategorized"} <p> This metric aligns with the @code availableReplicas @endcode field of the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicationcontrollerstatus-v1-core">K8s
 * ReplicationControllerStatus</a> <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#replicationcontroller">@code k8s.replicationcontroller @endcode</a>
 * resource. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sReplicationControllerAvailablePods =
    "k8s.replication_controller.available_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricK8sReplicationControllerAvailablePods =
        "Deprecated, use `k8s.replicationcontroller.available_pods` instead.";
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
 * Deprecated, use @code k8s.replicationcontroller.desired_pods @endcode instead.
 *
 * @deprecated
 * {"note": "Replaced by @code k8s.replicationcontroller.desired_pods @endcode.", "reason":
 * "uncategorized"} <p> This metric aligns with the @code replicas @endcode field of the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicationcontrollerspec-v1-core">K8s
 * ReplicationControllerSpec</a> <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#replicationcontroller">@code k8s.replicationcontroller @endcode</a>
 * resource. <p> updowncounter
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kMetricK8sReplicationControllerDesiredPods =
    "k8s.replication_controller.desired_pods";
OPENTELEMETRY_DEPRECATED static constexpr const char
    *descrMetricK8sReplicationControllerDesiredPods =
        "Deprecated, use `k8s.replicationcontroller.desired_pods` instead.";
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
 * Total number of available replica pods (ready for at least minReadySeconds) targeted by this
 * replication controller <p> This metric aligns with the @code availableReplicas @endcode field of
 * the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicationcontrollerstatus-v1-core">K8s
 * ReplicationControllerStatus</a> <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#replicationcontroller">@code k8s.replicationcontroller @endcode</a>
 * resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sReplicationcontrollerAvailablePods =
    "k8s.replicationcontroller.available_pods";
static constexpr const char *descrMetricK8sReplicationcontrollerAvailablePods =
    "Total number of available replica pods (ready for at least minReadySeconds) targeted by this "
    "replication controller";
static constexpr const char *unitMetricK8sReplicationcontrollerAvailablePods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicationcontrollerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicationcontrollerAvailablePods,
                                         descrMetricK8sReplicationcontrollerAvailablePods,
                                         unitMetricK8sReplicationcontrollerAvailablePods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicationcontrollerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicationcontrollerAvailablePods,
                                          descrMetricK8sReplicationcontrollerAvailablePods,
                                          unitMetricK8sReplicationcontrollerAvailablePods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicationcontrollerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicationcontrollerAvailablePods,
                                                   descrMetricK8sReplicationcontrollerAvailablePods,
                                                   unitMetricK8sReplicationcontrollerAvailablePods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicationcontrollerAvailablePods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricK8sReplicationcontrollerAvailablePods,
      descrMetricK8sReplicationcontrollerAvailablePods,
      unitMetricK8sReplicationcontrollerAvailablePods);
}

/**
 * Number of desired replica pods in this replication controller
 * <p>
 * This metric aligns with the @code replicas @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#replicationcontrollerspec-v1-core">K8s
 * ReplicationControllerSpec</a> <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#replicationcontroller">@code k8s.replicationcontroller @endcode</a>
 * resource. <p> updowncounter
 */
static constexpr const char *kMetricK8sReplicationcontrollerDesiredPods =
    "k8s.replicationcontroller.desired_pods";
static constexpr const char *descrMetricK8sReplicationcontrollerDesiredPods =
    "Number of desired replica pods in this replication controller";
static constexpr const char *unitMetricK8sReplicationcontrollerDesiredPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sReplicationcontrollerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sReplicationcontrollerDesiredPods,
                                         descrMetricK8sReplicationcontrollerDesiredPods,
                                         unitMetricK8sReplicationcontrollerDesiredPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sReplicationcontrollerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sReplicationcontrollerDesiredPods,
                                          descrMetricK8sReplicationcontrollerDesiredPods,
                                          unitMetricK8sReplicationcontrollerDesiredPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sReplicationcontrollerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sReplicationcontrollerDesiredPods,
                                                   descrMetricK8sReplicationcontrollerDesiredPods,
                                                   unitMetricK8sReplicationcontrollerDesiredPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sReplicationcontrollerDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sReplicationcontrollerDesiredPods,
                                                    descrMetricK8sReplicationcontrollerDesiredPods,
                                                    unitMetricK8sReplicationcontrollerDesiredPods);
}

/**
 * The number of replica pods created by the statefulset controller from the statefulset version
 * indicated by currentRevision <p> This metric aligns with the @code currentReplicas @endcode field
 * of the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetstatus-v1-apps">K8s
 * StatefulSetStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#statefulset">@code k8s.statefulset @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sStatefulsetCurrentPods = "k8s.statefulset.current_pods";
static constexpr const char *descrMetricK8sStatefulsetCurrentPods =
    "The number of replica pods created by the statefulset controller from the statefulset version "
    "indicated by currentRevision";
static constexpr const char *unitMetricK8sStatefulsetCurrentPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetCurrentPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetCurrentPods,
                                         descrMetricK8sStatefulsetCurrentPods,
                                         unitMetricK8sStatefulsetCurrentPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetCurrentPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetCurrentPods,
                                          descrMetricK8sStatefulsetCurrentPods,
                                          unitMetricK8sStatefulsetCurrentPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetCurrentPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetCurrentPods,
                                                   descrMetricK8sStatefulsetCurrentPods,
                                                   unitMetricK8sStatefulsetCurrentPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetCurrentPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetCurrentPods,
                                                    descrMetricK8sStatefulsetCurrentPods,
                                                    unitMetricK8sStatefulsetCurrentPods);
}

/**
 * Number of desired replica pods in this statefulset
 * <p>
 * This metric aligns with the @code replicas @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetspec-v1-apps">K8s
 * StatefulSetSpec</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#statefulset">@code k8s.statefulset @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sStatefulsetDesiredPods = "k8s.statefulset.desired_pods";
static constexpr const char *descrMetricK8sStatefulsetDesiredPods =
    "Number of desired replica pods in this statefulset";
static constexpr const char *unitMetricK8sStatefulsetDesiredPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetDesiredPods,
                                         descrMetricK8sStatefulsetDesiredPods,
                                         unitMetricK8sStatefulsetDesiredPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetDesiredPods,
                                          descrMetricK8sStatefulsetDesiredPods,
                                          unitMetricK8sStatefulsetDesiredPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetDesiredPods,
                                                   descrMetricK8sStatefulsetDesiredPods,
                                                   unitMetricK8sStatefulsetDesiredPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetDesiredPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetDesiredPods,
                                                    descrMetricK8sStatefulsetDesiredPods,
                                                    unitMetricK8sStatefulsetDesiredPods);
}

/**
 * The number of replica pods created for this statefulset with a Ready Condition
 * <p>
 * This metric aligns with the @code readyReplicas @endcode field of the
 * <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetstatus-v1-apps">K8s
 * StatefulSetStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#statefulset">@code k8s.statefulset @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sStatefulsetReadyPods = "k8s.statefulset.ready_pods";
static constexpr const char *descrMetricK8sStatefulsetReadyPods =
    "The number of replica pods created for this statefulset with a Ready Condition";
static constexpr const char *unitMetricK8sStatefulsetReadyPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetReadyPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetReadyPods,
                                         descrMetricK8sStatefulsetReadyPods,
                                         unitMetricK8sStatefulsetReadyPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetReadyPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetReadyPods,
                                          descrMetricK8sStatefulsetReadyPods,
                                          unitMetricK8sStatefulsetReadyPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetReadyPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetReadyPods,
                                                   descrMetricK8sStatefulsetReadyPods,
                                                   unitMetricK8sStatefulsetReadyPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetReadyPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetReadyPods,
                                                    descrMetricK8sStatefulsetReadyPods,
                                                    unitMetricK8sStatefulsetReadyPods);
}

/**
 * Number of replica pods created by the statefulset controller from the statefulset version
 * indicated by updateRevision <p> This metric aligns with the @code updatedReplicas @endcode field
 * of the <a
 * href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#statefulsetstatus-v1-apps">K8s
 * StatefulSetStatus</a>. <p> This metric SHOULD, at a minimum, be reported against a <a
 * href="../resource/k8s.md#statefulset">@code k8s.statefulset @endcode</a> resource. <p>
 * updowncounter
 */
static constexpr const char *kMetricK8sStatefulsetUpdatedPods = "k8s.statefulset.updated_pods";
static constexpr const char *descrMetricK8sStatefulsetUpdatedPods =
    "Number of replica pods created by the statefulset controller from the statefulset version "
    "indicated by updateRevision";
static constexpr const char *unitMetricK8sStatefulsetUpdatedPods = "{pod}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricK8sStatefulsetUpdatedPods(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(kMetricK8sStatefulsetUpdatedPods,
                                         descrMetricK8sStatefulsetUpdatedPods,
                                         unitMetricK8sStatefulsetUpdatedPods);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricK8sStatefulsetUpdatedPods(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(kMetricK8sStatefulsetUpdatedPods,
                                          descrMetricK8sStatefulsetUpdatedPods,
                                          unitMetricK8sStatefulsetUpdatedPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricK8sStatefulsetUpdatedPods(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(kMetricK8sStatefulsetUpdatedPods,
                                                   descrMetricK8sStatefulsetUpdatedPods,
                                                   unitMetricK8sStatefulsetUpdatedPods);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricK8sStatefulsetUpdatedPods(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(kMetricK8sStatefulsetUpdatedPods,
                                                    descrMetricK8sStatefulsetUpdatedPods,
                                                    unitMetricK8sStatefulsetUpdatedPods);
}

}  // namespace k8s
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
