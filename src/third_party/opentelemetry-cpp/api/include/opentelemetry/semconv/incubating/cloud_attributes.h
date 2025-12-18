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
namespace cloud
{

/**
  The cloud account ID the resource is assigned to.
 */
static constexpr const char *kCloudAccountId = "cloud.account.id";

/**
  Cloud regions often have multiple, isolated locations known as zones to increase availability.
  Availability zone represents the zone where the resource is running. <p> Availability zones are
  called "zones" on Alibaba Cloud and Google Cloud.
 */
static constexpr const char *kCloudAvailabilityZone = "cloud.availability_zone";

/**
  The cloud platform in use.
  <p>
  The prefix of the service SHOULD match the one specified in @code cloud.provider @endcode.
 */
static constexpr const char *kCloudPlatform = "cloud.platform";

/**
  Name of the cloud provider.
 */
static constexpr const char *kCloudProvider = "cloud.provider";

/**
  The geographical region within a cloud provider. When associated with a resource, this attribute
  specifies the region where the resource operates. When calling services or APIs deployed on a
  cloud, this attribute identifies the region where the called destination is deployed. <p> Refer to
  your provider's docs to see the available regions, for example <a
  href="https://www.alibabacloud.com/help/doc-detail/40654.htm">Alibaba Cloud regions</a>, <a
  href="https://aws.amazon.com/about-aws/global-infrastructure/regions_az/">AWS regions</a>, <a
  href="https://azure.microsoft.com/global-infrastructure/geographies/">Azure regions</a>, <a
  href="https://cloud.google.com/about/locations">Google Cloud regions</a>, or <a
  href="https://www.tencentcloud.com/document/product/213/6091">Tencent Cloud regions</a>.
 */
static constexpr const char *kCloudRegion = "cloud.region";

/**
  Cloud provider-specific native identifier of the monitored cloud resource (e.g. an <a
  href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">ARN</a> on AWS,
  a <a href="https://learn.microsoft.com/rest/api/resources/resources/get-by-id">fully qualified
  resource ID</a> on Azure, a <a href="https://google.aip.dev/122#full-resource-names">full resource
  name</a> on GCP) <p> On some cloud providers, it may not be possible to determine the full ID at
  startup, so it may be necessary to set @code cloud.resource_id @endcode as a span attribute
  instead. <p> The exact value to use for @code cloud.resource_id @endcode depends on the cloud
  provider. The following well-known definitions MUST be used if you set this attribute and they
  apply: <ul> <li><strong>AWS Lambda:</strong> The function <a
  href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">ARN</a>. Take
  care not to use the "invoked ARN" directly but replace any <a
  href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-aliases.html">alias suffix</a>
  with the resolved function version, as the same runtime instance may be invocable with
  multiple different aliases.</li>
    <li><strong>GCP:</strong> The <a
  href="https://cloud.google.com/iam/docs/full-resource-names">URI of the resource</a></li>
    <li><strong>Azure:</strong> The <a
  href="https://learn.microsoft.com/rest/api/resources/resources/get-by-id">Fully Qualified Resource
  ID</a> of the invoked function, <em>not</em> the function app, having the form
  @code
  /subscriptions/<SUBSCRIPTION_GUID>/resourceGroups/<RG>/providers/Microsoft.Web/sites/<FUNCAPP>/functions/<FUNC>
  @endcode. This means that a span attribute MUST be used, as an Azure function app can host
  multiple functions that would usually share a TracerProvider.</li>
  </ul>
 */
static constexpr const char *kCloudResourceId = "cloud.resource_id";

namespace CloudPlatformValues
{
/**
  Alibaba Cloud Elastic Compute Service
 */
static constexpr const char *kAlibabaCloudEcs = "alibaba_cloud_ecs";

/**
  Alibaba Cloud Function Compute
 */
static constexpr const char *kAlibabaCloudFc = "alibaba_cloud_fc";

/**
  Red Hat OpenShift on Alibaba Cloud
 */
static constexpr const char *kAlibabaCloudOpenshift = "alibaba_cloud_openshift";

/**
  AWS Elastic Compute Cloud
 */
static constexpr const char *kAwsEc2 = "aws_ec2";

/**
  AWS Elastic Container Service
 */
static constexpr const char *kAwsEcs = "aws_ecs";

/**
  AWS Elastic Kubernetes Service
 */
static constexpr const char *kAwsEks = "aws_eks";

/**
  AWS Lambda
 */
static constexpr const char *kAwsLambda = "aws_lambda";

/**
  AWS Elastic Beanstalk
 */
static constexpr const char *kAwsElasticBeanstalk = "aws_elastic_beanstalk";

/**
  AWS App Runner
 */
static constexpr const char *kAwsAppRunner = "aws_app_runner";

/**
  Red Hat OpenShift on AWS (ROSA)
 */
static constexpr const char *kAwsOpenshift = "aws_openshift";

/**
  Azure Virtual Machines
 */
static constexpr const char *kAzureVm = "azure.vm";

/**
  Azure Container Apps
 */
static constexpr const char *kAzureContainerApps = "azure.container_apps";

/**
  Azure Container Instances
 */
static constexpr const char *kAzureContainerInstances = "azure.container_instances";

/**
  Azure Kubernetes Service
 */
static constexpr const char *kAzureAks = "azure.aks";

/**
  Azure Functions
 */
static constexpr const char *kAzureFunctions = "azure.functions";

/**
  Azure App Service
 */
static constexpr const char *kAzureAppService = "azure.app_service";

/**
  Azure Red Hat OpenShift
 */
static constexpr const char *kAzureOpenshift = "azure.openshift";

/**
  Google Bare Metal Solution (BMS)
 */
static constexpr const char *kGcpBareMetalSolution = "gcp_bare_metal_solution";

/**
  Google Cloud Compute Engine (GCE)
 */
static constexpr const char *kGcpComputeEngine = "gcp_compute_engine";

/**
  Google Cloud Run
 */
static constexpr const char *kGcpCloudRun = "gcp_cloud_run";

/**
  Google Cloud Kubernetes Engine (GKE)
 */
static constexpr const char *kGcpKubernetesEngine = "gcp_kubernetes_engine";

/**
  Google Cloud Functions (GCF)
 */
static constexpr const char *kGcpCloudFunctions = "gcp_cloud_functions";

/**
  Google Cloud App Engine (GAE)
 */
static constexpr const char *kGcpAppEngine = "gcp_app_engine";

/**
  Red Hat OpenShift on Google Cloud
 */
static constexpr const char *kGcpOpenshift = "gcp_openshift";

/**
  Red Hat OpenShift on IBM Cloud
 */
static constexpr const char *kIbmCloudOpenshift = "ibm_cloud_openshift";

/**
  Compute on Oracle Cloud Infrastructure (OCI)
 */
static constexpr const char *kOracleCloudCompute = "oracle_cloud_compute";

/**
  Kubernetes Engine (OKE) on Oracle Cloud Infrastructure (OCI)
 */
static constexpr const char *kOracleCloudOke = "oracle_cloud_oke";

/**
  Tencent Cloud Cloud Virtual Machine (CVM)
 */
static constexpr const char *kTencentCloudCvm = "tencent_cloud_cvm";

/**
  Tencent Cloud Elastic Kubernetes Service (EKS)
 */
static constexpr const char *kTencentCloudEks = "tencent_cloud_eks";

/**
  Tencent Cloud Serverless Cloud Function (SCF)
 */
static constexpr const char *kTencentCloudScf = "tencent_cloud_scf";

}  // namespace CloudPlatformValues

namespace CloudProviderValues
{
/**
  Alibaba Cloud
 */
static constexpr const char *kAlibabaCloud = "alibaba_cloud";

/**
  Amazon Web Services
 */
static constexpr const char *kAws = "aws";

/**
  Microsoft Azure
 */
static constexpr const char *kAzure = "azure";

/**
  Google Cloud Platform
 */
static constexpr const char *kGcp = "gcp";

/**
  Heroku Platform as a Service
 */
static constexpr const char *kHeroku = "heroku";

/**
  IBM Cloud
 */
static constexpr const char *kIbmCloud = "ibm_cloud";

/**
  Oracle Cloud Infrastructure (OCI)
 */
static constexpr const char *kOracleCloud = "oracle_cloud";

/**
  Tencent Cloud
 */
static constexpr const char *kTencentCloud = "tencent_cloud";

}  // namespace CloudProviderValues

}  // namespace cloud
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
