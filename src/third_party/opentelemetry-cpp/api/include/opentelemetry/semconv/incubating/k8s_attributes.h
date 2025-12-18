/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace k8s
{

/**
  The name of the cluster.
 */
static constexpr const char *kK8sClusterName = "k8s.cluster.name";

/**
  A pseudo-ID for the cluster, set to the UID of the @code kube-system @endcode namespace.
  <p>
  K8s doesn't have support for obtaining a cluster ID. If this is ever
  added, we will recommend collecting the @code k8s.cluster.uid @endcode through the
  official APIs. In the meantime, we are able to use the @code uid @endcode of the
  @code kube-system @endcode namespace as a proxy for cluster ID. Read on for the
  rationale.
  <p>
  Every object created in a K8s cluster is assigned a distinct UID. The
  @code kube-system @endcode namespace is used by Kubernetes itself and will exist
  for the lifetime of the cluster. Using the @code uid @endcode of the @code kube-system @endcode
  namespace is a reasonable proxy for the K8s ClusterID as it will only
  change if the cluster is rebuilt. Furthermore, Kubernetes UIDs are
  UUIDs as standardized by
  <a href="https://www.itu.int/ITU-T/studygroups/com17/oid.html">ISO/IEC 9834-8 and ITU-T X.667</a>.
  Which states:
  <blockquote>
  If generated according to one of the mechanisms defined in Rec.
  ITU-T X.667 | ISO/IEC 9834-8, a UUID is either guaranteed to be
  different from all other UUIDs generated before 3603 A.D., or is
  extremely likely to be different (depending on the mechanism chosen).</blockquote>
  <p>
  Therefore, UIDs between clusters should be extremely unlikely to
  conflict.
 */
static constexpr const char *kK8sClusterUid = "k8s.cluster.uid";

/**
  The name of the Container from Pod specification, must be unique within a Pod. Container runtime
  usually uses different globally unique name (@code container.name @endcode).
 */
static constexpr const char *kK8sContainerName = "k8s.container.name";

/**
  Number of times the container was restarted. This attribute can be used to identify a particular
  container (running or stopped) within a container spec.
 */
static constexpr const char *kK8sContainerRestartCount = "k8s.container.restart_count";

/**
  Last terminated reason of the Container.
 */
static constexpr const char *kK8sContainerStatusLastTerminatedReason =
    "k8s.container.status.last_terminated_reason";

/**
  The reason for the container state. Corresponds to the @code reason @endcode field of the: <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#containerstatewaiting-v1-core">K8s
  ContainerStateWaiting</a> or <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#containerstateterminated-v1-core">K8s
  ContainerStateTerminated</a>
 */
static constexpr const char *kK8sContainerStatusReason = "k8s.container.status.reason";

/**
  The state of the container. <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#containerstate-v1-core">K8s
  ContainerState</a>
 */
static constexpr const char *kK8sContainerStatusState = "k8s.container.status.state";

/**
  The cronjob annotation placed on the CronJob, the @code <key> @endcode being the annotation name,
  the value being the annotation value. <p> Examples: <ul> <li>An annotation @code retries @endcode
  with value @code 4 @endcode SHOULD be recorded as the
  @code k8s.cronjob.annotation.retries @endcode attribute with value @code "4" @endcode.</li>
    <li>An annotation @code data @endcode with empty string value SHOULD be recorded as
  the @code k8s.cronjob.annotation.data @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sCronjobAnnotation = "k8s.cronjob.annotation";

/**
  The label placed on the CronJob, the @code <key> @endcode being the label name, the value being
  the label value. <p> Examples: <ul> <li>A label @code type @endcode with value @code weekly
  @endcode SHOULD be recorded as the
  @code k8s.cronjob.label.type @endcode attribute with value @code "weekly" @endcode.</li>
    <li>A label @code automated @endcode with empty string value SHOULD be recorded as
  the @code k8s.cronjob.label.automated @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sCronjobLabel = "k8s.cronjob.label";

/**
  The name of the CronJob.
 */
static constexpr const char *kK8sCronjobName = "k8s.cronjob.name";

/**
  The UID of the CronJob.
 */
static constexpr const char *kK8sCronjobUid = "k8s.cronjob.uid";

/**
  The annotation placed on the DaemonSet, the @code <key> @endcode being the annotation name, the
  value being the annotation value, even if the value is empty. <p> Examples: <ul> <li>A label @code
  replicas @endcode with value @code 1 @endcode SHOULD be recorded as the @code
  k8s.daemonset.annotation.replicas @endcode attribute with value @code "1" @endcode.</li> <li>A
  label @code data @endcode with empty string value SHOULD be recorded as the @code
  k8s.daemonset.annotation.data @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sDaemonsetAnnotation = "k8s.daemonset.annotation";

/**
  The label placed on the DaemonSet, the @code <key> @endcode being the label name, the value being
  the label value, even if the value is empty. <p> Examples: <ul> <li>A label @code app @endcode
  with value @code guestbook @endcode SHOULD be recorded as the @code k8s.daemonset.label.app
  @endcode attribute with value @code "guestbook" @endcode.</li> <li>A label @code data @endcode
  with empty string value SHOULD be recorded as the @code k8s.daemonset.label.injected @endcode
  attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sDaemonsetLabel = "k8s.daemonset.label";

/**
  The name of the DaemonSet.
 */
static constexpr const char *kK8sDaemonsetName = "k8s.daemonset.name";

/**
  The UID of the DaemonSet.
 */
static constexpr const char *kK8sDaemonsetUid = "k8s.daemonset.uid";

/**
  The annotation placed on the Deployment, the @code <key> @endcode being the annotation name, the
  value being the annotation value, even if the value is empty. <p> Examples: <ul> <li>A label @code
  replicas @endcode with value @code 1 @endcode SHOULD be recorded as the @code
  k8s.deployment.annotation.replicas @endcode attribute with value @code "1" @endcode.</li> <li>A
  label @code data @endcode with empty string value SHOULD be recorded as the @code
  k8s.deployment.annotation.data @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sDeploymentAnnotation = "k8s.deployment.annotation";

/**
  The label placed on the Deployment, the @code <key> @endcode being the label name, the value being
  the label value, even if the value is empty. <p> Examples: <ul> <li>A label @code replicas
  @endcode with value @code 0 @endcode SHOULD be recorded as the @code k8s.deployment.label.app
  @endcode attribute with value @code "guestbook" @endcode.</li> <li>A label @code injected @endcode
  with empty string value SHOULD be recorded as the @code k8s.deployment.label.injected @endcode
  attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sDeploymentLabel = "k8s.deployment.label";

/**
  The name of the Deployment.
 */
static constexpr const char *kK8sDeploymentName = "k8s.deployment.name";

/**
  The UID of the Deployment.
 */
static constexpr const char *kK8sDeploymentUid = "k8s.deployment.uid";

/**
  The type of metric source for the horizontal pod autoscaler.
  <p>
  This attribute reflects the @code type @endcode field of spec.metrics[] in the HPA.
 */
static constexpr const char *kK8sHpaMetricType = "k8s.hpa.metric.type";

/**
  The name of the horizontal pod autoscaler.
 */
static constexpr const char *kK8sHpaName = "k8s.hpa.name";

/**
  The API version of the target resource to scale for the HorizontalPodAutoscaler.
  <p>
  This maps to the @code apiVersion @endcode field in the @code scaleTargetRef @endcode of the HPA
  spec.
 */
static constexpr const char *kK8sHpaScaletargetrefApiVersion = "k8s.hpa.scaletargetref.api_version";

/**
  The kind of the target resource to scale for the HorizontalPodAutoscaler.
  <p>
  This maps to the @code kind @endcode field in the @code scaleTargetRef @endcode of the HPA spec.
 */
static constexpr const char *kK8sHpaScaletargetrefKind = "k8s.hpa.scaletargetref.kind";

/**
  The name of the target resource to scale for the HorizontalPodAutoscaler.
  <p>
  This maps to the @code name @endcode field in the @code scaleTargetRef @endcode of the HPA spec.
 */
static constexpr const char *kK8sHpaScaletargetrefName = "k8s.hpa.scaletargetref.name";

/**
  The UID of the horizontal pod autoscaler.
 */
static constexpr const char *kK8sHpaUid = "k8s.hpa.uid";

/**
  The size (identifier) of the K8s huge page.
 */
static constexpr const char *kK8sHugepageSize = "k8s.hugepage.size";

/**
  The annotation placed on the Job, the @code <key> @endcode being the annotation name, the value
  being the annotation value, even if the value is empty. <p> Examples: <ul> <li>A label @code
  number @endcode with value @code 1 @endcode SHOULD be recorded as the @code
  k8s.job.annotation.number @endcode attribute with value @code "1" @endcode.</li> <li>A label @code
  data @endcode with empty string value SHOULD be recorded as the @code k8s.job.annotation.data
  @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sJobAnnotation = "k8s.job.annotation";

/**
  The label placed on the Job, the @code <key> @endcode being the label name, the value being the
  label value, even if the value is empty. <p> Examples: <ul> <li>A label @code jobtype @endcode
  with value @code ci @endcode SHOULD be recorded as the @code k8s.job.label.jobtype @endcode
  attribute with value @code "ci" @endcode.</li> <li>A label @code data @endcode with empty string
  value SHOULD be recorded as the @code k8s.job.label.automated @endcode attribute with value @code
  "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sJobLabel = "k8s.job.label";

/**
  The name of the Job.
 */
static constexpr const char *kK8sJobName = "k8s.job.name";

/**
  The UID of the Job.
 */
static constexpr const char *kK8sJobUid = "k8s.job.uid";

/**
  The annotation placed on the Namespace, the @code <key> @endcode being the annotation name, the
  value being the annotation value, even if the value is empty. <p> Examples: <ul> <li>A label @code
  ttl @endcode with value @code 0 @endcode SHOULD be recorded as the @code
  k8s.namespace.annotation.ttl @endcode attribute with value @code "0" @endcode.</li> <li>A label
  @code data @endcode with empty string value SHOULD be recorded as the @code
  k8s.namespace.annotation.data @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sNamespaceAnnotation = "k8s.namespace.annotation";

/**
  The label placed on the Namespace, the @code <key> @endcode being the label name, the value being
  the label value, even if the value is empty. <p> Examples: <ul> <li>A label @code
  kubernetes.io/metadata.name @endcode with value @code default @endcode SHOULD be recorded as the
  @code k8s.namespace.label.kubernetes.io/metadata.name @endcode attribute with value @code
  "default" @endcode.</li> <li>A label @code data @endcode with empty string value SHOULD be
  recorded as the @code k8s.namespace.label.data @endcode attribute with value @code ""
  @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sNamespaceLabel = "k8s.namespace.label";

/**
  The name of the namespace that the pod is running in.
 */
static constexpr const char *kK8sNamespaceName = "k8s.namespace.name";

/**
  The phase of the K8s namespace.
  <p>
  This attribute aligns with the @code phase @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#namespacestatus-v1-core">K8s
  NamespaceStatus</a>
 */
static constexpr const char *kK8sNamespacePhase = "k8s.namespace.phase";

/**
  The annotation placed on the Node, the @code <key> @endcode being the annotation name, the value
  being the annotation value, even if the value is empty. <p> Examples: <ul> <li>An annotation @code
  node.alpha.kubernetes.io/ttl @endcode with value @code 0 @endcode SHOULD be recorded as the @code
  k8s.node.annotation.node.alpha.kubernetes.io/ttl @endcode attribute with value @code "0"
  @endcode.</li> <li>An annotation @code data @endcode with empty string value SHOULD be recorded as
  the @code k8s.node.annotation.data @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sNodeAnnotation = "k8s.node.annotation";

/**
  The status of the condition, one of True, False, Unknown.
  <p>
  This attribute aligns with the @code status @endcode field of the
  <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#nodecondition-v1-core">NodeCondition</a>
 */
static constexpr const char *kK8sNodeConditionStatus = "k8s.node.condition.status";

/**
  The condition type of a K8s Node.
  <p>
  K8s Node conditions as described
  by <a href="https://v1-32.docs.kubernetes.io/docs/reference/node/node-status/#condition">K8s
  documentation</a>. <p> This attribute aligns with the @code type @endcode field of the <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#nodecondition-v1-core">NodeCondition</a>
  <p>
  The set of possible values is not limited to those listed here. Managed Kubernetes environments,
  or custom controllers MAY introduce additional node condition types.
  When this occurs, the exact value as reported by the Kubernetes API SHOULD be used.
 */
static constexpr const char *kK8sNodeConditionType = "k8s.node.condition.type";

/**
  The label placed on the Node, the @code <key> @endcode being the label name, the value being the
  label value, even if the value is empty. <p> Examples: <ul> <li>A label @code kubernetes.io/arch
  @endcode with value @code arm64 @endcode SHOULD be recorded as the @code
  k8s.node.label.kubernetes.io/arch @endcode attribute with value @code "arm64" @endcode.</li> <li>A
  label @code data @endcode with empty string value SHOULD be recorded as the @code
  k8s.node.label.data @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sNodeLabel = "k8s.node.label";

/**
  The name of the Node.
 */
static constexpr const char *kK8sNodeName = "k8s.node.name";

/**
  The UID of the Node.
 */
static constexpr const char *kK8sNodeUid = "k8s.node.uid";

/**
  The annotation placed on the Pod, the @code <key> @endcode being the annotation name, the value
  being the annotation value. <p> Examples: <ul> <li>An annotation @code
  kubernetes.io/enforce-mountable-secrets @endcode with value @code true @endcode SHOULD be recorded
  as the @code k8s.pod.annotation.kubernetes.io/enforce-mountable-secrets @endcode attribute with
  value @code "true" @endcode.</li> <li>An annotation @code mycompany.io/arch @endcode with value
  @code x64 @endcode SHOULD be recorded as the @code k8s.pod.annotation.mycompany.io/arch @endcode
  attribute with value @code "x64" @endcode.</li> <li>An annotation @code data @endcode with empty
  string value SHOULD be recorded as the @code k8s.pod.annotation.data @endcode attribute with value
  @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sPodAnnotation = "k8s.pod.annotation";

/**
  The label placed on the Pod, the @code <key> @endcode being the label name, the value being the
  label value. <p> Examples: <ul> <li>A label @code app @endcode with value @code my-app @endcode
  SHOULD be recorded as the @code k8s.pod.label.app @endcode attribute with value @code "my-app"
  @endcode.</li> <li>A label @code mycompany.io/arch @endcode with value @code x64 @endcode SHOULD
  be recorded as the @code k8s.pod.label.mycompany.io/arch @endcode attribute with value @code "x64"
  @endcode.</li> <li>A label @code data @endcode with empty string value SHOULD be recorded as the
  @code k8s.pod.label.data @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sPodLabel = "k8s.pod.label";

/**
  Deprecated, use @code k8s.pod.label @endcode instead.

  @deprecated
  {"note": "Replaced by @code k8s.pod.label @endcode.", "reason": "renamed", "renamed_to":
  "k8s.pod.label"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kK8sPodLabels = "k8s.pod.labels";

/**
  The name of the Pod.
 */
static constexpr const char *kK8sPodName = "k8s.pod.name";

/**
  The phase for the pod. Corresponds to the @code phase @endcode field of the: <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.33/#podstatus-v1-core">K8s
  PodStatus</a>
 */
static constexpr const char *kK8sPodStatusPhase = "k8s.pod.status.phase";

/**
  The reason for the pod state. Corresponds to the @code reason @endcode field of the: <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.33/#podstatus-v1-core">K8s
  PodStatus</a>
 */
static constexpr const char *kK8sPodStatusReason = "k8s.pod.status.reason";

/**
  The UID of the Pod.
 */
static constexpr const char *kK8sPodUid = "k8s.pod.uid";

/**
  The annotation placed on the ReplicaSet, the @code <key> @endcode being the annotation name, the
  value being the annotation value, even if the value is empty. <p> Examples: <ul> <li>A label @code
  replicas @endcode with value @code 0 @endcode SHOULD be recorded as the @code
  k8s.replicaset.annotation.replicas @endcode attribute with value @code "0" @endcode.</li> <li>A
  label @code data @endcode with empty string value SHOULD be recorded as the @code
  k8s.replicaset.annotation.data @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sReplicasetAnnotation = "k8s.replicaset.annotation";

/**
  The label placed on the ReplicaSet, the @code <key> @endcode being the label name, the value being
  the label value, even if the value is empty. <p> Examples: <ul> <li>A label @code app @endcode
  with value @code guestbook @endcode SHOULD be recorded as the @code k8s.replicaset.label.app
  @endcode attribute with value @code "guestbook" @endcode.</li> <li>A label @code injected @endcode
  with empty string value SHOULD be recorded as the @code k8s.replicaset.label.injected @endcode
  attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sReplicasetLabel = "k8s.replicaset.label";

/**
  The name of the ReplicaSet.
 */
static constexpr const char *kK8sReplicasetName = "k8s.replicaset.name";

/**
  The UID of the ReplicaSet.
 */
static constexpr const char *kK8sReplicasetUid = "k8s.replicaset.uid";

/**
  The name of the replication controller.
 */
static constexpr const char *kK8sReplicationcontrollerName = "k8s.replicationcontroller.name";

/**
  The UID of the replication controller.
 */
static constexpr const char *kK8sReplicationcontrollerUid = "k8s.replicationcontroller.uid";

/**
  The name of the resource quota.
 */
static constexpr const char *kK8sResourcequotaName = "k8s.resourcequota.name";

/**
  The name of the K8s resource a resource quota defines.
  <p>
  The value for this attribute can be either the full @code count/<resource>[.<group>] @endcode
  string (e.g., count/deployments.apps, count/pods), or, for certain core Kubernetes resources, just
  the resource name (e.g., pods, services, configmaps). Both forms are supported by Kubernetes for
  object count quotas. See <a
  href="https://kubernetes.io/docs/concepts/policy/resource-quotas/#quota-on-object-count">Kubernetes
  Resource Quotas documentation</a> for more details.
 */
static constexpr const char *kK8sResourcequotaResourceName = "k8s.resourcequota.resource_name";

/**
  The UID of the resource quota.
 */
static constexpr const char *kK8sResourcequotaUid = "k8s.resourcequota.uid";

/**
  The annotation placed on the StatefulSet, the @code <key> @endcode being the annotation name, the
  value being the annotation value, even if the value is empty. <p> Examples: <ul> <li>A label @code
  replicas @endcode with value @code 1 @endcode SHOULD be recorded as the @code
  k8s.statefulset.annotation.replicas @endcode attribute with value @code "1" @endcode.</li> <li>A
  label @code data @endcode with empty string value SHOULD be recorded as the @code
  k8s.statefulset.annotation.data @endcode attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sStatefulsetAnnotation = "k8s.statefulset.annotation";

/**
  The label placed on the StatefulSet, the @code <key> @endcode being the label name, the value
  being the label value, even if the value is empty. <p> Examples: <ul> <li>A label @code replicas
  @endcode with value @code 0 @endcode SHOULD be recorded as the @code k8s.statefulset.label.app
  @endcode attribute with value @code "guestbook" @endcode.</li> <li>A label @code injected @endcode
  with empty string value SHOULD be recorded as the @code k8s.statefulset.label.injected @endcode
  attribute with value @code "" @endcode.</li>
  </ul>
 */
static constexpr const char *kK8sStatefulsetLabel = "k8s.statefulset.label";

/**
  The name of the StatefulSet.
 */
static constexpr const char *kK8sStatefulsetName = "k8s.statefulset.name";

/**
  The UID of the StatefulSet.
 */
static constexpr const char *kK8sStatefulsetUid = "k8s.statefulset.uid";

/**
  The name of K8s <a
  href="https://kubernetes.io/docs/reference/generated/kubernetes-api/v1.30/#storageclass-v1-storage-k8s-io">StorageClass</a>
  object.
 */
static constexpr const char *kK8sStorageclassName = "k8s.storageclass.name";

/**
  The name of the K8s volume.
 */
static constexpr const char *kK8sVolumeName = "k8s.volume.name";

/**
  The type of the K8s volume.
 */
static constexpr const char *kK8sVolumeType = "k8s.volume.type";

namespace K8sContainerStatusReasonValues
{
/**
  The container is being created.
 */
static constexpr const char *kContainerCreating = "ContainerCreating";

/**
  The container is in a crash loop back off state.
 */
static constexpr const char *kCrashLoopBackOff = "CrashLoopBackOff";

/**
  There was an error creating the container configuration.
 */
static constexpr const char *kCreateContainerConfigError = "CreateContainerConfigError";

/**
  There was an error pulling the container image.
 */
static constexpr const char *kErrImagePull = "ErrImagePull";

/**
  The container image pull is in back off state.
 */
static constexpr const char *kImagePullBackOff = "ImagePullBackOff";

/**
  The container was killed due to out of memory.
 */
static constexpr const char *kOomKilled = "OOMKilled";

/**
  The container has completed execution.
 */
static constexpr const char *kCompleted = "Completed";

/**
  There was an error with the container.
 */
static constexpr const char *kError = "Error";

/**
  The container cannot run.
 */
static constexpr const char *kContainerCannotRun = "ContainerCannotRun";

}  // namespace K8sContainerStatusReasonValues

namespace K8sContainerStatusStateValues
{
/**
  The container has terminated.
 */
static constexpr const char *kTerminated = "terminated";

/**
  The container is running.
 */
static constexpr const char *kRunning = "running";

/**
  The container is waiting.
 */
static constexpr const char *kWaiting = "waiting";

}  // namespace K8sContainerStatusStateValues

namespace K8sNamespacePhaseValues
{
/**
  Active namespace phase as described by <a
  href="https://pkg.go.dev/k8s.io/api@v0.31.3/core/v1#NamespacePhase">K8s API</a>
 */
static constexpr const char *kActive = "active";

/**
  Terminating namespace phase as described by <a
  href="https://pkg.go.dev/k8s.io/api@v0.31.3/core/v1#NamespacePhase">K8s API</a>
 */
static constexpr const char *kTerminating = "terminating";

}  // namespace K8sNamespacePhaseValues

namespace K8sNodeConditionStatusValues
{

static constexpr const char *kConditionTrue = "true";

static constexpr const char *kConditionFalse = "false";

static constexpr const char *kConditionUnknown = "unknown";

}  // namespace K8sNodeConditionStatusValues

namespace K8sNodeConditionTypeValues
{
/**
  The node is healthy and ready to accept pods
 */
static constexpr const char *kReady = "Ready";

/**
  Pressure exists on the disk size—that is, if the disk capacity is low
 */
static constexpr const char *kDiskPressure = "DiskPressure";

/**
  Pressure exists on the node memory—that is, if the node memory is low
 */
static constexpr const char *kMemoryPressure = "MemoryPressure";

/**
  Pressure exists on the processes—that is, if there are too many processes on the node
 */
static constexpr const char *kPidPressure = "PIDPressure";

/**
  The network for the node is not correctly configured
 */
static constexpr const char *kNetworkUnavailable = "NetworkUnavailable";

}  // namespace K8sNodeConditionTypeValues

namespace K8sPodStatusPhaseValues
{
/**
  The pod has been accepted by the system, but one or more of the containers has not been started.
  This includes time before being bound to a node, as well as time spent pulling images onto the
  host.
 */
static constexpr const char *kPending = "Pending";

/**
  The pod has been bound to a node and all of the containers have been started. At least one
  container is still running or is in the process of being restarted.
 */
static constexpr const char *kRunning = "Running";

/**
  All containers in the pod have voluntarily terminated with a container exit code of 0, and the
  system is not going to restart any of these containers.
 */
static constexpr const char *kSucceeded = "Succeeded";

/**
  All containers in the pod have terminated, and at least one container has terminated in a failure
  (exited with a non-zero exit code or was stopped by the system).
 */
static constexpr const char *kFailed = "Failed";

/**
  For some reason the state of the pod could not be obtained, typically due to an error in
  communicating with the host of the pod.
 */
static constexpr const char *kUnknown = "Unknown";

}  // namespace K8sPodStatusPhaseValues

namespace K8sPodStatusReasonValues
{
/**
  The pod is evicted.
 */
static constexpr const char *kEvicted = "Evicted";

/**
  The pod is in a status because of its node affinity
 */
static constexpr const char *kNodeAffinity = "NodeAffinity";

/**
  The reason on a pod when its state cannot be confirmed as kubelet is unresponsive on the node it
  is (was) running.
 */
static constexpr const char *kNodeLost = "NodeLost";

/**
  The node is shutdown
 */
static constexpr const char *kShutdown = "Shutdown";

/**
  The pod was rejected admission to the node because of an error during admission that could not be
  categorized.
 */
static constexpr const char *kUnexpectedAdmissionError = "UnexpectedAdmissionError";

}  // namespace K8sPodStatusReasonValues

namespace K8sVolumeTypeValues
{
/**
  A <a
  href="https://v1-30.docs.kubernetes.io/docs/concepts/storage/volumes/#persistentvolumeclaim">persistentVolumeClaim</a>
  volume
 */
static constexpr const char *kPersistentVolumeClaim = "persistentVolumeClaim";

/**
  A <a
  href="https://v1-30.docs.kubernetes.io/docs/concepts/storage/volumes/#configmap">configMap</a>
  volume
 */
static constexpr const char *kConfigMap = "configMap";

/**
  A <a
  href="https://v1-30.docs.kubernetes.io/docs/concepts/storage/volumes/#downwardapi">downwardAPI</a>
  volume
 */
static constexpr const char *kDownwardApi = "downwardAPI";

/**
  An <a href="https://v1-30.docs.kubernetes.io/docs/concepts/storage/volumes/#emptydir">emptyDir</a>
  volume
 */
static constexpr const char *kEmptyDir = "emptyDir";

/**
  A <a href="https://v1-30.docs.kubernetes.io/docs/concepts/storage/volumes/#secret">secret</a>
  volume
 */
static constexpr const char *kSecret = "secret";

/**
  A <a href="https://v1-30.docs.kubernetes.io/docs/concepts/storage/volumes/#local">local</a> volume
 */
static constexpr const char *kLocal = "local";

}  // namespace K8sVolumeTypeValues

}  // namespace k8s
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
