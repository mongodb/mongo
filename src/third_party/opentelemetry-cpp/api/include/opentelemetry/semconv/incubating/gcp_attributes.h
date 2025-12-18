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
namespace gcp
{

/**
  The container within GCP where the AppHub application is defined.
 */
static constexpr const char *kGcpApphubApplicationContainer = "gcp.apphub.application.container";

/**
  The name of the application as configured in AppHub.
 */
static constexpr const char *kGcpApphubApplicationId = "gcp.apphub.application.id";

/**
  The GCP zone or region where the application is defined.
 */
static constexpr const char *kGcpApphubApplicationLocation = "gcp.apphub.application.location";

/**
  Criticality of a service indicates its importance to the business.
  <p>
  <a href="https://cloud.google.com/app-hub/docs/reference/rest/v1/Attributes#type">See AppHub type
  enum</a>
 */
static constexpr const char *kGcpApphubServiceCriticalityType =
    "gcp.apphub.service.criticality_type";

/**
  Environment of a service is the stage of a software lifecycle.
  <p>
  <a href="https://cloud.google.com/app-hub/docs/reference/rest/v1/Attributes#type_1">See AppHub
  environment type</a>
 */
static constexpr const char *kGcpApphubServiceEnvironmentType =
    "gcp.apphub.service.environment_type";

/**
  The name of the service as configured in AppHub.
 */
static constexpr const char *kGcpApphubServiceId = "gcp.apphub.service.id";

/**
  Criticality of a workload indicates its importance to the business.
  <p>
  <a href="https://cloud.google.com/app-hub/docs/reference/rest/v1/Attributes#type">See AppHub type
  enum</a>
 */
static constexpr const char *kGcpApphubWorkloadCriticalityType =
    "gcp.apphub.workload.criticality_type";

/**
  Environment of a workload is the stage of a software lifecycle.
  <p>
  <a href="https://cloud.google.com/app-hub/docs/reference/rest/v1/Attributes#type_1">See AppHub
  environment type</a>
 */
static constexpr const char *kGcpApphubWorkloadEnvironmentType =
    "gcp.apphub.workload.environment_type";

/**
  The name of the workload as configured in AppHub.
 */
static constexpr const char *kGcpApphubWorkloadId = "gcp.apphub.workload.id";

/**
  The container within GCP where the AppHub destination application is defined.
 */
static constexpr const char *kGcpApphubDestinationApplicationContainer =
    "gcp.apphub_destination.application.container";

/**
  The name of the destination application as configured in AppHub.
 */
static constexpr const char *kGcpApphubDestinationApplicationId =
    "gcp.apphub_destination.application.id";

/**
  The GCP zone or region where the destination application is defined.
 */
static constexpr const char *kGcpApphubDestinationApplicationLocation =
    "gcp.apphub_destination.application.location";

/**
  Criticality of a destination workload indicates its importance to the business as specified in <a
  href="https://cloud.google.com/app-hub/docs/reference/rest/v1/Attributes#type">AppHub type
  enum</a>
 */
static constexpr const char *kGcpApphubDestinationServiceCriticalityType =
    "gcp.apphub_destination.service.criticality_type";

/**
  Software lifecycle stage of a destination service as defined <a
  href="https://cloud.google.com/app-hub/docs/reference/rest/v1/Attributes#type_1">AppHub
  environment type</a>
 */
static constexpr const char *kGcpApphubDestinationServiceEnvironmentType =
    "gcp.apphub_destination.service.environment_type";

/**
  The name of the destination service as configured in AppHub.
 */
static constexpr const char *kGcpApphubDestinationServiceId = "gcp.apphub_destination.service.id";

/**
  Criticality of a destination workload indicates its importance to the business as specified in <a
  href="https://cloud.google.com/app-hub/docs/reference/rest/v1/Attributes#type">AppHub type
  enum</a>
 */
static constexpr const char *kGcpApphubDestinationWorkloadCriticalityType =
    "gcp.apphub_destination.workload.criticality_type";

/**
  Environment of a destination workload is the stage of a software lifecycle as provided in the <a
  href="https://cloud.google.com/app-hub/docs/reference/rest/v1/Attributes#type_1">AppHub
  environment type</a>
 */
static constexpr const char *kGcpApphubDestinationWorkloadEnvironmentType =
    "gcp.apphub_destination.workload.environment_type";

/**
  The name of the destination workload as configured in AppHub.
 */
static constexpr const char *kGcpApphubDestinationWorkloadId = "gcp.apphub_destination.workload.id";

/**
  Identifies the Google Cloud service for which the official client library is intended.
  <p>
  Intended to be a stable identifier for Google Cloud client libraries that is uniform across
  implementation languages. The value should be derived from the canonical service domain for the
  service; for example, 'foo.googleapis.com' should result in a value of 'foo'.
 */
static constexpr const char *kGcpClientService = "gcp.client.service";

/**
  The name of the Cloud Run <a
  href="https://cloud.google.com/run/docs/managing/job-executions">execution</a> being run for the
  Job, as set by the <a
  href="https://cloud.google.com/run/docs/container-contract#jobs-env-vars">@code
  CLOUD_RUN_EXECUTION @endcode</a> environment variable.
 */
static constexpr const char *kGcpCloudRunJobExecution = "gcp.cloud_run.job.execution";

/**
  The index for a task within an execution as provided by the <a
  href="https://cloud.google.com/run/docs/container-contract#jobs-env-vars">@code
  CLOUD_RUN_TASK_INDEX @endcode</a> environment variable.
 */
static constexpr const char *kGcpCloudRunJobTaskIndex = "gcp.cloud_run.job.task_index";

/**
  The hostname of a GCE instance. This is the full value of the default or <a
  href="https://cloud.google.com/compute/docs/instances/custom-hostname-vm">custom hostname</a>.
 */
static constexpr const char *kGcpGceInstanceHostname = "gcp.gce.instance.hostname";

/**
  The instance name of a GCE instance. This is the value provided by @code host.name @endcode, the
  visible name of the instance in the Cloud Console UI, and the prefix for the default hostname of
  the instance as defined by the <a
  href="https://cloud.google.com/compute/docs/internal-dns#instance-fully-qualified-domain-names">default
  internal DNS name</a>.
 */
static constexpr const char *kGcpGceInstanceName = "gcp.gce.instance.name";

namespace GcpApphubServiceCriticalityTypeValues
{
/**
  Mission critical service.
 */
static constexpr const char *kMissionCritical = "MISSION_CRITICAL";

/**
  High impact.
 */
static constexpr const char *kHigh = "HIGH";

/**
  Medium impact.
 */
static constexpr const char *kMedium = "MEDIUM";

/**
  Low impact.
 */
static constexpr const char *kLow = "LOW";

}  // namespace GcpApphubServiceCriticalityTypeValues

namespace GcpApphubServiceEnvironmentTypeValues
{
/**
  Production environment.
 */
static constexpr const char *kProduction = "PRODUCTION";

/**
  Staging environment.
 */
static constexpr const char *kStaging = "STAGING";

/**
  Test environment.
 */
static constexpr const char *kTest = "TEST";

/**
  Development environment.
 */
static constexpr const char *kDevelopment = "DEVELOPMENT";

}  // namespace GcpApphubServiceEnvironmentTypeValues

namespace GcpApphubWorkloadCriticalityTypeValues
{
/**
  Mission critical service.
 */
static constexpr const char *kMissionCritical = "MISSION_CRITICAL";

/**
  High impact.
 */
static constexpr const char *kHigh = "HIGH";

/**
  Medium impact.
 */
static constexpr const char *kMedium = "MEDIUM";

/**
  Low impact.
 */
static constexpr const char *kLow = "LOW";

}  // namespace GcpApphubWorkloadCriticalityTypeValues

namespace GcpApphubWorkloadEnvironmentTypeValues
{
/**
  Production environment.
 */
static constexpr const char *kProduction = "PRODUCTION";

/**
  Staging environment.
 */
static constexpr const char *kStaging = "STAGING";

/**
  Test environment.
 */
static constexpr const char *kTest = "TEST";

/**
  Development environment.
 */
static constexpr const char *kDevelopment = "DEVELOPMENT";

}  // namespace GcpApphubWorkloadEnvironmentTypeValues

namespace GcpApphubDestinationServiceCriticalityTypeValues
{
/**
  Mission critical service.
 */
static constexpr const char *kMissionCritical = "MISSION_CRITICAL";

/**
  High impact.
 */
static constexpr const char *kHigh = "HIGH";

/**
  Medium impact.
 */
static constexpr const char *kMedium = "MEDIUM";

/**
  Low impact.
 */
static constexpr const char *kLow = "LOW";

}  // namespace GcpApphubDestinationServiceCriticalityTypeValues

namespace GcpApphubDestinationServiceEnvironmentTypeValues
{
/**
  Production environment.
 */
static constexpr const char *kProduction = "PRODUCTION";

/**
  Staging environment.
 */
static constexpr const char *kStaging = "STAGING";

/**
  Test environment.
 */
static constexpr const char *kTest = "TEST";

/**
  Development environment.
 */
static constexpr const char *kDevelopment = "DEVELOPMENT";

}  // namespace GcpApphubDestinationServiceEnvironmentTypeValues

namespace GcpApphubDestinationWorkloadCriticalityTypeValues
{
/**
  Mission critical service.
 */
static constexpr const char *kMissionCritical = "MISSION_CRITICAL";

/**
  High impact.
 */
static constexpr const char *kHigh = "HIGH";

/**
  Medium impact.
 */
static constexpr const char *kMedium = "MEDIUM";

/**
  Low impact.
 */
static constexpr const char *kLow = "LOW";

}  // namespace GcpApphubDestinationWorkloadCriticalityTypeValues

namespace GcpApphubDestinationWorkloadEnvironmentTypeValues
{
/**
  Production environment.
 */
static constexpr const char *kProduction = "PRODUCTION";

/**
  Staging environment.
 */
static constexpr const char *kStaging = "STAGING";

/**
  Test environment.
 */
static constexpr const char *kTest = "TEST";

/**
  Development environment.
 */
static constexpr const char *kDevelopment = "DEVELOPMENT";

}  // namespace GcpApphubDestinationWorkloadEnvironmentTypeValues

}  // namespace gcp
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
