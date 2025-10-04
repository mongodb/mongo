/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
  DO NOT EDIT, this is an Auto-generated file
  from buildscripts/semantic-convention/templates/SemanticAttributes.h.j2
*/

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace trace
{

namespace SemanticConventions
{
/**
 * The URL of the OpenTelemetry schema for these keys and values.
 */
static constexpr const char *kSchemaUrl = "https://opentelemetry.io/schemas/1.27.0";

/**
 * Uniquely identifies the framework API revision offered by a version ({@code os.version}) of the
 * android operating system. More information can be found <a
 * href="https://developer.android.com/guide/topics/manifest/uses-sdk-element#ApiLevels">here</a>.
 */
static constexpr const char *kAndroidOsApiLevel = "android.os.api_level";

/**
 * The provenance filename of the built attestation which directly relates to the build artifact
 * filename. This filename SHOULD accompany the artifact at publish time. See the <a
 * href="https://slsa.dev/spec/v1.0/distributing-provenance#relationship-between-artifacts-and-attestations">SLSA
 * Relationship</a> specification for more information.
 */
static constexpr const char *kArtifactAttestationFilename = "artifact.attestation.filename";

/**
 * The full <a href="https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.186-5.pdf">hash value (see
 * glossary)</a>, of the built attestation. Some envelopes in the software attestation space also
 * refer to this as the <a
 * href="https://github.com/in-toto/attestation/blob/main/spec/README.md#in-toto-attestation-framework-spec">digest</a>.
 */
static constexpr const char *kArtifactAttestationHash = "artifact.attestation.hash";

/**
 * The id of the build <a href="https://slsa.dev/attestation-model">software attestation</a>.
 */
static constexpr const char *kArtifactAttestationId = "artifact.attestation.id";

/**
 * The human readable file name of the artifact, typically generated during build and release
processes. Often includes the package name and version in the file name.
 *
 * <p>Notes:
  <ul> <li>This file name can also act as the <a
href="https://slsa.dev/spec/v1.0/terminology#package-model">Package Name</a> in cases where the
package ecosystem maps accordingly. Additionally, the artifact <a
href="https://slsa.dev/spec/v1.0/terminology#software-supply-chain">can be published</a> for others,
but that is not a guarantee.</li> </ul>
 */
static constexpr const char *kArtifactFilename = "artifact.filename";

/**
 * The full <a href="https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.186-5.pdf">hash value (see
glossary)</a>, often found in checksum.txt on a release of the artifact and used to verify package
integrity.
 *
 * <p>Notes:
  <ul> <li>The specific algorithm used to create the cryptographic hash value is
not defined. In situations where an artifact has multiple
cryptographic hashes, it is up to the implementer to choose which
hash value to set here; this should be the most secure hash algorithm
that is suitable for the situation and consistent with the
corresponding attestation. The implementer can then provide the other
hash values through an additional set of attribute extensions as they
deem necessary.</li> </ul>
 */
static constexpr const char *kArtifactHash = "artifact.hash";

/**
 * The <a href="https://github.com/package-url/purl-spec">Package URL</a> of the <a
 * href="https://slsa.dev/spec/v1.0/terminology#package-model">package artifact</a> provides a
 * standard way to identify and locate the packaged artifact.
 */
static constexpr const char *kArtifactPurl = "artifact.purl";

/**
 * The version of the artifact.
 */
static constexpr const char *kArtifactVersion = "artifact.version";

/**
 * Rate-limiting result, shows whether the lease was acquired or contains a rejection reason
 */
static constexpr const char *kAspnetcoreRateLimitingResult = "aspnetcore.rate_limiting.result";

/**
 * Full type name of the <a
 * href="https://learn.microsoft.com/dotnet/api/microsoft.aspnetcore.diagnostics.iexceptionhandler">{@code
 * IExceptionHandler}</a> implementation that handled the exception.
 */
static constexpr const char *kAspnetcoreDiagnosticsHandlerType =
    "aspnetcore.diagnostics.handler.type";

/**
 * ASP.NET Core exception middleware handling result
 */
static constexpr const char *kAspnetcoreDiagnosticsExceptionResult =
    "aspnetcore.diagnostics.exception.result";

/**
 * Rate limiting policy name.
 */
static constexpr const char *kAspnetcoreRateLimitingPolicy = "aspnetcore.rate_limiting.policy";

/**
 * Flag indicating if request was handled by the application pipeline.
 */
static constexpr const char *kAspnetcoreRequestIsUnhandled = "aspnetcore.request.is_unhandled";

/**
 * A value that indicates whether the matched route is a fallback route.
 */
static constexpr const char *kAspnetcoreRoutingIsFallback = "aspnetcore.routing.is_fallback";

/**
 * Match result - success or failure
 */
static constexpr const char *kAspnetcoreRoutingMatchStatus = "aspnetcore.routing.match_status";

/**
 * The AWS request ID as returned in the response headers {@code x-amz-request-id} or {@code
 * x-amz-requestid}.
 */
static constexpr const char *kAwsRequestId = "aws.request_id";

/**
 * The JSON-serialized value of each item in the {@code AttributeDefinitions} request field.
 */
static constexpr const char *kAwsDynamodbAttributeDefinitions =
    "aws.dynamodb.attribute_definitions";

/**
 * The value of the {@code AttributesToGet} request parameter.
 */
static constexpr const char *kAwsDynamodbAttributesToGet = "aws.dynamodb.attributes_to_get";

/**
 * The value of the {@code ConsistentRead} request parameter.
 */
static constexpr const char *kAwsDynamodbConsistentRead = "aws.dynamodb.consistent_read";

/**
 * The JSON-serialized value of each item in the {@code ConsumedCapacity} response field.
 */
static constexpr const char *kAwsDynamodbConsumedCapacity = "aws.dynamodb.consumed_capacity";

/**
 * The value of the {@code Count} response parameter.
 */
static constexpr const char *kAwsDynamodbCount = "aws.dynamodb.count";

/**
 * The value of the {@code ExclusiveStartTableName} request parameter.
 */
static constexpr const char *kAwsDynamodbExclusiveStartTable = "aws.dynamodb.exclusive_start_table";

/**
 * The JSON-serialized value of each item in the {@code GlobalSecondaryIndexUpdates} request field.
 */
static constexpr const char *kAwsDynamodbGlobalSecondaryIndexUpdates =
    "aws.dynamodb.global_secondary_index_updates";

/**
 * The JSON-serialized value of each item of the {@code GlobalSecondaryIndexes} request field
 */
static constexpr const char *kAwsDynamodbGlobalSecondaryIndexes =
    "aws.dynamodb.global_secondary_indexes";

/**
 * The value of the {@code IndexName} request parameter.
 */
static constexpr const char *kAwsDynamodbIndexName = "aws.dynamodb.index_name";

/**
 * The JSON-serialized value of the {@code ItemCollectionMetrics} response field.
 */
static constexpr const char *kAwsDynamodbItemCollectionMetrics =
    "aws.dynamodb.item_collection_metrics";

/**
 * The value of the {@code Limit} request parameter.
 */
static constexpr const char *kAwsDynamodbLimit = "aws.dynamodb.limit";

/**
 * The JSON-serialized value of each item of the {@code LocalSecondaryIndexes} request field.
 */
static constexpr const char *kAwsDynamodbLocalSecondaryIndexes =
    "aws.dynamodb.local_secondary_indexes";

/**
 * The value of the {@code ProjectionExpression} request parameter.
 */
static constexpr const char *kAwsDynamodbProjection = "aws.dynamodb.projection";

/**
 * The value of the {@code ProvisionedThroughput.ReadCapacityUnits} request parameter.
 */
static constexpr const char *kAwsDynamodbProvisionedReadCapacity =
    "aws.dynamodb.provisioned_read_capacity";

/**
 * The value of the {@code ProvisionedThroughput.WriteCapacityUnits} request parameter.
 */
static constexpr const char *kAwsDynamodbProvisionedWriteCapacity =
    "aws.dynamodb.provisioned_write_capacity";

/**
 * The value of the {@code ScanIndexForward} request parameter.
 */
static constexpr const char *kAwsDynamodbScanForward = "aws.dynamodb.scan_forward";

/**
 * The value of the {@code ScannedCount} response parameter.
 */
static constexpr const char *kAwsDynamodbScannedCount = "aws.dynamodb.scanned_count";

/**
 * The value of the {@code Segment} request parameter.
 */
static constexpr const char *kAwsDynamodbSegment = "aws.dynamodb.segment";

/**
 * The value of the {@code Select} request parameter.
 */
static constexpr const char *kAwsDynamodbSelect = "aws.dynamodb.select";

/**
 * The number of items in the {@code TableNames} response parameter.
 */
static constexpr const char *kAwsDynamodbTableCount = "aws.dynamodb.table_count";

/**
 * The keys in the {@code RequestItems} object field.
 */
static constexpr const char *kAwsDynamodbTableNames = "aws.dynamodb.table_names";

/**
 * The value of the {@code TotalSegments} request parameter.
 */
static constexpr const char *kAwsDynamodbTotalSegments = "aws.dynamodb.total_segments";

/**
 * The ID of a running ECS task. The ID MUST be extracted from {@code task.arn}.
 */
static constexpr const char *kAwsEcsTaskId = "aws.ecs.task.id";

/**
 * The ARN of an <a
 * href="https://docs.aws.amazon.com/AmazonECS/latest/developerguide/clusters.html">ECS cluster</a>.
 */
static constexpr const char *kAwsEcsClusterArn = "aws.ecs.cluster.arn";

/**
 * The Amazon Resource Name (ARN) of an <a
 * href="https://docs.aws.amazon.com/AmazonECS/latest/developerguide/ECS_instances.html">ECS
 * container instance</a>.
 */
static constexpr const char *kAwsEcsContainerArn = "aws.ecs.container.arn";

/**
 * The <a
 * href="https://docs.aws.amazon.com/AmazonECS/latest/developerguide/launch_types.html">launch
 * type</a> for an ECS task.
 */
static constexpr const char *kAwsEcsLaunchtype = "aws.ecs.launchtype";

/**
 * The ARN of a running <a
 * href="https://docs.aws.amazon.com/AmazonECS/latest/developerguide/ecs-account-settings.html#ecs-resource-ids">ECS
 * task</a>.
 */
static constexpr const char *kAwsEcsTaskArn = "aws.ecs.task.arn";

/**
 * The family name of the <a
 * href="https://docs.aws.amazon.com/AmazonECS/latest/developerguide/task_definitions.html">ECS task
 * definition</a> used to create the ECS task.
 */
static constexpr const char *kAwsEcsTaskFamily = "aws.ecs.task.family";

/**
 * The revision for the task definition used to create the ECS task.
 */
static constexpr const char *kAwsEcsTaskRevision = "aws.ecs.task.revision";

/**
 * The ARN of an EKS cluster.
 */
static constexpr const char *kAwsEksClusterArn = "aws.eks.cluster.arn";

/**
 * The Amazon Resource Name(s) (ARN) of the AWS log group(s).
 *
 * <p>Notes:
  <ul> <li>See the <a
 href="https://docs.aws.amazon.com/AmazonCloudWatch/latest/logs/iam-access-control-overview-cwl.html#CWL_ARN_Format">log
 group ARN format documentation</a>.</li> </ul>
 */
static constexpr const char *kAwsLogGroupArns = "aws.log.group.arns";

/**
 * The name(s) of the AWS log group(s) an application is writing to.
 *
 * <p>Notes:
  <ul> <li>Multiple log groups must be supported for cases like multi-container applications, where
 a single application has sidecar containers, and each write to their own log group.</li> </ul>
 */
static constexpr const char *kAwsLogGroupNames = "aws.log.group.names";

/**
 * The ARN(s) of the AWS log stream(s).
 *
 * <p>Notes:
  <ul> <li>See the <a
 href="https://docs.aws.amazon.com/AmazonCloudWatch/latest/logs/iam-access-control-overview-cwl.html#CWL_ARN_Format">log
 stream ARN format documentation</a>. One log group can contain several log streams, so these ARNs
 necessarily identify both a log group and a log stream.</li> </ul>
 */
static constexpr const char *kAwsLogStreamArns = "aws.log.stream.arns";

/**
 * The name(s) of the AWS log stream(s) an application is writing to.
 */
static constexpr const char *kAwsLogStreamNames = "aws.log.stream.names";

/**
 * The full invoked ARN as provided on the {@code Context} passed to the function ({@code
 Lambda-Runtime-Invoked-Function-Arn} header on the {@code /runtime/invocation/next} applicable).
 *
 * <p>Notes:
  <ul> <li>This may be different from {@code cloud.resource_id} if an alias is involved.</li> </ul>
 */
static constexpr const char *kAwsLambdaInvokedArn = "aws.lambda.invoked_arn";

/**
 * The S3 bucket name the request refers to. Corresponds to the {@code --bucket} parameter of the <a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/index.html">S3 API</a> operations.
 *
 * <p>Notes:
  <ul> <li>The {@code bucket} attribute is applicable to all S3 operations that reference a bucket,
i.e. that require the bucket name as a mandatory parameter. This applies to almost all S3 operations
except {@code list-buckets}.</li> </ul>
 */
static constexpr const char *kAwsS3Bucket = "aws.s3.bucket";

/**
 * The source object (in the form {@code bucket}/{@code key}) for the copy operation.
 *
 * <p>Notes:
  <ul> <li>The {@code copy_source} attribute applies to S3 copy operations and corresponds to the
{@code --copy-source} parameter of the <a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/copy-object.html">copy-object operation
within the S3 API</a>. This applies in particular to the following operations:</li><li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/copy-object.html">copy-object</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part-copy.html">upload-part-copy</a></li>
 </ul>
 */
static constexpr const char *kAwsS3CopySource = "aws.s3.copy_source";

/**
 * The delete request container that specifies the objects to be deleted.
 *
 * <p>Notes:
  <ul> <li>The {@code delete} attribute is only applicable to the <a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/delete-object.html">delete-object</a>
operation. The {@code delete} attribute corresponds to the {@code --delete} parameter of the <a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/delete-objects.html">delete-objects
operation within the S3 API</a>.</li> </ul>
 */
static constexpr const char *kAwsS3Delete = "aws.s3.delete";

/**
 * The S3 object key the request refers to. Corresponds to the {@code --key} parameter of the <a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/index.html">S3 API</a> operations.
 *
 * <p>Notes:
  <ul> <li>The {@code key} attribute is applicable to all object-related S3 operations, i.e. that
require the object key as a mandatory parameter. This applies in particular to the following
operations:</li><li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/copy-object.html">copy-object</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/delete-object.html">delete-object</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/get-object.html">get-object</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/head-object.html">head-object</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/put-object.html">put-object</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/restore-object.html">restore-object</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/select-object-content.html">select-object-content</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/abort-multipart-upload.html">abort-multipart-upload</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/complete-multipart-upload.html">complete-multipart-upload</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/create-multipart-upload.html">create-multipart-upload</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/list-parts.html">list-parts</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part.html">upload-part</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part-copy.html">upload-part-copy</a></li>
 </ul>
 */
static constexpr const char *kAwsS3Key = "aws.s3.key";

/**
 * The part number of the part being uploaded in a multipart-upload operation. This is a positive
integer between 1 and 10,000.
 *
 * <p>Notes:
  <ul> <li>The {@code part_number} attribute is only applicable to the <a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part.html">upload-part</a> and
<a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part-copy.html">upload-part-copy</a>
operations. The {@code part_number} attribute corresponds to the {@code --part-number} parameter of
the <a href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part.html">upload-part
operation within the S3 API</a>.</li> </ul>
 */
static constexpr const char *kAwsS3PartNumber = "aws.s3.part_number";

/**
 * Upload ID that identifies the multipart upload.
 *
 * <p>Notes:
  <ul> <li>The {@code upload_id} attribute applies to S3 multipart-upload operations and corresponds
to the {@code --upload-id} parameter of the <a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/index.html">S3 API</a> multipart
operations. This applies in particular to the following operations:</li><li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/abort-multipart-upload.html">abort-multipart-upload</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/complete-multipart-upload.html">complete-multipart-upload</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/list-parts.html">list-parts</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part.html">upload-part</a></li>
<li><a
href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part-copy.html">upload-part-copy</a></li>
 </ul>
 */
static constexpr const char *kAwsS3UploadId = "aws.s3.upload_id";

/**
 * The unique identifier of the service request. It's generated by the Azure service and returned
 * with the response.
 */
static constexpr const char *kAzServiceRequestId = "az.service_request_id";

/**
 * Array of brand name and version separated by a space
 *
 * <p>Notes:
  <ul> <li>This value is intended to be taken from the <a
 href="https://wicg.github.io/ua-client-hints/#interface">UA client hints API</a> ({@code
 navigator.userAgentData.brands}).</li> </ul>
 */
static constexpr const char *kBrowserBrands = "browser.brands";

/**
 * Preferred language of the user using the browser
 *
 * <p>Notes:
  <ul> <li>This value is intended to be taken from the Navigator API {@code
 navigator.language}.</li> </ul>
 */
static constexpr const char *kBrowserLanguage = "browser.language";

/**
 * A boolean that is true if the browser is running on a mobile device
 *
 * <p>Notes:
  <ul> <li>This value is intended to be taken from the <a
 href="https://wicg.github.io/ua-client-hints/#interface">UA client hints API</a> ({@code
 navigator.userAgentData.mobile}). If unavailable, this attribute SHOULD be left unset.</li> </ul>
 */
static constexpr const char *kBrowserMobile = "browser.mobile";

/**
 * The platform on which the browser is running
 *
 * <p>Notes:
  <ul> <li>This value is intended to be taken from the <a
href="https://wicg.github.io/ua-client-hints/#interface">UA client hints API</a> ({@code
navigator.userAgentData.platform}). If unavailable, the legacy {@code navigator.platform} API SHOULD
NOT be used instead and this attribute SHOULD be left unset in order for the values to be
consistent. The list of possible values is defined in the <a
href="https://wicg.github.io/ua-client-hints/#sec-ch-ua-platform">W3C User-Agent Client Hints
specification</a>. Note that some (but not all) of these values can overlap with values in the <a
href="./os.md">{@code os.type} and {@code os.name} attributes</a>. However, for consistency, the
values in the {@code browser.platform} attribute should capture the exact value that the user agent
provides.</li> </ul>
 */
static constexpr const char *kBrowserPlatform = "browser.platform";

/**
 * The human readable name of the pipeline within a CI/CD system.
 */
static constexpr const char *kCicdPipelineName = "cicd.pipeline.name";

/**
 * The unique identifier of a pipeline run within a CI/CD system.
 */
static constexpr const char *kCicdPipelineRunId = "cicd.pipeline.run.id";

/**
 * The human readable name of a task within a pipeline. Task here most closely aligns with a <a
 * href="https://en.wikipedia.org/wiki/Pipeline_(computing)">computing process</a> in a pipeline.
 * Other terms for tasks include commands, steps, and procedures.
 */
static constexpr const char *kCicdPipelineTaskName = "cicd.pipeline.task.name";

/**
 * The unique identifier of a task run within a pipeline.
 */
static constexpr const char *kCicdPipelineTaskRunId = "cicd.pipeline.task.run.id";

/**
 * The <a href="https://en.wikipedia.org/wiki/URL">URL</a> of the pipeline run providing the
 * complete address in order to locate and identify the pipeline run.
 */
static constexpr const char *kCicdPipelineTaskRunUrlFull = "cicd.pipeline.task.run.url.full";

/**
 * The type of the task within a pipeline.
 */
static constexpr const char *kCicdPipelineTaskType = "cicd.pipeline.task.type";

/**
 * Client address - domain name if available without reverse DNS lookup; otherwise, IP address or
 Unix domain socket name.
 *
 * <p>Notes:
  <ul> <li>When observed from the server side, and when communicating through an intermediary,
 {@code client.address} SHOULD represent the client address behind any intermediaries,  for example
 proxies, if it's available.</li> </ul>
 */
static constexpr const char *kClientAddress = "client.address";

/**
 * Client port number.
 *
 * <p>Notes:
  <ul> <li>When observed from the server side, and when communicating through an intermediary,
 {@code client.port} SHOULD represent the client port behind any intermediaries,  for example
 proxies, if it's available.</li> </ul>
 */
static constexpr const char *kClientPort = "client.port";

/**
 * The cloud account ID the resource is assigned to.
 */
static constexpr const char *kCloudAccountId = "cloud.account.id";

/**
 * Cloud regions often have multiple, isolated locations known as zones to increase availability.
 Availability zone represents the zone where the resource is running.
 *
 * <p>Notes:
  <ul> <li>Availability zones are called &quot;zones&quot; on Alibaba Cloud and Google Cloud.</li>
 </ul>
 */
static constexpr const char *kCloudAvailabilityZone = "cloud.availability_zone";

/**
 * The cloud platform in use.
 *
 * <p>Notes:
  <ul> <li>The prefix of the service SHOULD match the one specified in {@code cloud.provider}.</li>
 </ul>
 */
static constexpr const char *kCloudPlatform = "cloud.platform";

/**
 * Name of the cloud provider.
 */
static constexpr const char *kCloudProvider = "cloud.provider";

/**
 * The geographical region the resource is running.
 *
 * <p>Notes:
  <ul> <li>Refer to your provider's docs to see the available regions, for example <a
 href="https://www.alibabacloud.com/help/doc-detail/40654.htm">Alibaba Cloud regions</a>, <a
 href="https://aws.amazon.com/about-aws/global-infrastructure/regions_az/">AWS regions</a>, <a
 href="https://azure.microsoft.com/global-infrastructure/geographies/">Azure regions</a>, <a
 href="https://cloud.google.com/about/locations">Google Cloud regions</a>, or <a
 href="https://www.tencentcloud.com/document/product/213/6091">Tencent Cloud regions</a>.</li> </ul>
 */
static constexpr const char *kCloudRegion = "cloud.region";

/**
 * Cloud provider-specific native identifier of the monitored cloud resource (e.g. an <a
href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">ARN</a> on AWS, a
<a href="https://learn.microsoft.com/rest/api/resources/resources/get-by-id">fully qualified
resource ID</a> on Azure, a <a
href="https://cloud.google.com/apis/design/resource_names#full_resource_name">full resource name</a>
on GCP)
 *
 * <p>Notes:
  <ul> <li>On some cloud providers, it may not be possible to determine the full ID at startup,
so it may be necessary to set {@code cloud.resource_id} as a span attribute instead.</li><li>The
exact value to use for {@code cloud.resource_id} depends on the cloud provider. The following
well-known definitions MUST be used if you set this attribute and they apply:</li><li><strong>AWS
Lambda:</strong> The function <a
href="https://docs.aws.amazon.com/general/latest/gr/aws-arns-and-namespaces.html">ARN</a>. Take care
not to use the &quot;invoked ARN&quot; directly but replace any <a
href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-aliases.html">alias suffix</a> with
the resolved function version, as the same runtime instance may be invocable with multiple different
aliases.</li> <li><strong>GCP:</strong> The <a
href="https://cloud.google.com/iam/docs/full-resource-names">URI of the resource</a></li>
<li><strong>Azure:</strong> The <a
href="https://docs.microsoft.com/rest/api/resources/resources/get-by-id">Fully Qualified Resource
ID</a> of the invoked function, <em>not</em> the function app, having the form
{@code
/subscriptions/<SUBSCIPTION_GUID>/resourceGroups/<RG>/providers/Microsoft.Web/sites/<FUNCAPP>/functions/<FUNC>}.
This means that a span attribute MUST be used, as an Azure function app can host multiple functions
that would usually share a TracerProvider.</li>
 </ul>
 */
static constexpr const char *kCloudResourceId = "cloud.resource_id";

/**
 * The <a href="https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md#id">event_id</a>
 * uniquely identifies the event.
 */
static constexpr const char *kCloudeventsEventId = "cloudevents.event_id";

/**
 * The <a
 * href="https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md#source-1">source</a>
 * identifies the context in which an event happened.
 */
static constexpr const char *kCloudeventsEventSource = "cloudevents.event_source";

/**
 * The <a
 * href="https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md#specversion">version of
 * the CloudEvents specification</a> which the event uses.
 */
static constexpr const char *kCloudeventsEventSpecVersion = "cloudevents.event_spec_version";

/**
 * The <a
 * href="https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md#subject">subject</a> of
 * the event in the context of the event producer (identified by source).
 */
static constexpr const char *kCloudeventsEventSubject = "cloudevents.event_subject";

/**
 * The <a
 * href="https://github.com/cloudevents/spec/blob/v1.0.2/cloudevents/spec.md#type">event_type</a>
 * contains a value describing the type of event related to the originating occurrence.
 */
static constexpr const char *kCloudeventsEventType = "cloudevents.event_type";

/**
 * The column number in {@code code.filepath} best representing the operation. It SHOULD point
 * within the code unit named in {@code code.function}.
 */
static constexpr const char *kCodeColumn = "code.column";

/**
 * The source code file name that identifies the code unit as uniquely as possible (preferably an
 * absolute file path).
 */
static constexpr const char *kCodeFilepath = "code.filepath";

/**
 * The method or function name, or equivalent (usually rightmost part of the code unit's name).
 */
static constexpr const char *kCodeFunction = "code.function";

/**
 * The line number in {@code code.filepath} best representing the operation. It SHOULD point within
 * the code unit named in {@code code.function}.
 */
static constexpr const char *kCodeLineno = "code.lineno";

/**
 * The &quot;namespace&quot; within which {@code code.function} is defined. Usually the qualified
 * class or module name, such that {@code code.namespace} + some separator + {@code code.function}
 * form a unique identifier for the code unit.
 */
static constexpr const char *kCodeNamespace = "code.namespace";

/**
 * A stacktrace as a string in the natural representation for the language runtime. The
 * representation is to be determined and documented by each language SIG.
 */
static constexpr const char *kCodeStacktrace = "code.stacktrace";

/**
 * The command used to run the container (i.e. the command name).
 *
 * <p>Notes:
  <ul> <li>If using embedded credentials or sensitive data, it is recommended to remove them to
 prevent potential leakage.</li> </ul>
 */
static constexpr const char *kContainerCommand = "container.command";

/**
 * All the command arguments (including the command/executable itself) run by the container. [2]
 */
static constexpr const char *kContainerCommandArgs = "container.command_args";

/**
 * The full command run by the container as a single string representing the full command. [2]
 */
static constexpr const char *kContainerCommandLine = "container.command_line";

/**
 * Container ID. Usually a UUID, as for example used to <a
 * href="https://docs.docker.com/engine/reference/run/#container-identification">identify Docker
 * containers</a>. The UUID might be abbreviated.
 */
static constexpr const char *kContainerId = "container.id";

/**
 * Runtime specific image identifier. Usually a hash algorithm followed by a UUID.
 *
 * <p>Notes:
  <ul> <li>Docker defines a sha256 of the image id; {@code container.image.id} corresponds to the
{@code Image} field from the Docker container inspect <a
href="https://docs.docker.com/engine/api/v1.43/#tag/Container/operation/ContainerInspect">API</a>
endpoint. K8s defines a link to the container registry repository with digest {@code "imageID":
"registry.azurecr.io
/namespace/service/dockerfile@sha256:bdeabd40c3a8a492eaf9e8e44d0ebbb84bac7ee25ac0cf8a7159d25f62555625"}.
The ID is assigned by the container runtime and can vary in different environments. Consider using
{@code oci.manifest.digest} if it is important to identify the same image in different
environments/runtimes.</li> </ul>
 */
static constexpr const char *kContainerImageId = "container.image.id";

/**
 * Name of the image the container was built on.
 */
static constexpr const char *kContainerImageName = "container.image.name";

/**
 * Repo digests of the container image as provided by the container runtime.
 *
 * <p>Notes:
  <ul> <li><a
 href="https://docs.docker.com/engine/api/v1.43/#tag/Image/operation/ImageInspect">Docker</a> and <a
 href="https://github.com/kubernetes/cri-api/blob/c75ef5b473bbe2d0a4fc92f82235efd665ea8e9f/pkg/apis/runtime/v1/api.proto#L1237-L1238">CRI</a>
 report those under the {@code RepoDigests} field.</li> </ul>
 */
static constexpr const char *kContainerImageRepoDigests = "container.image.repo_digests";

/**
 * Container image tags. An example can be found in <a
 * href="https://docs.docker.com/engine/api/v1.43/#tag/Image/operation/ImageInspect">Docker Image
 * Inspect</a>. Should be only the {@code <tag>} section of the full name for example from {@code
 * registry.example.com/my-org/my-image:<tag>}.
 */
static constexpr const char *kContainerImageTags = "container.image.tags";

/**
 * Container name used by container runtime.
 */
static constexpr const char *kContainerName = "container.name";

/**
 * The container runtime managing this container.
 */
static constexpr const char *kContainerRuntime = "container.runtime";

/**
 * The mode of the CPU
 */
static constexpr const char *kCpuMode = "cpu.mode";

/**
 * The name of the connection pool; unique within the instrumented application. In case the
 * connection pool implementation doesn't provide a name, instrumentation SHOULD use a combination
 * of parameters that would make the name unique, for example, combining attributes {@code
 * server.address}, {@code server.port}, and {@code db.namespace}, formatted as {@code
 * server.address:server.port/db.namespace}. Instrumentations that generate connection pool name
 * following different patterns SHOULD document it.
 */
static constexpr const char *kDbClientConnectionPoolName = "db.client.connection.pool.name";

/**
 * The state of a connection in the pool
 */
static constexpr const char *kDbClientConnectionState = "db.client.connection.state";

/**
 * The name of a collection (table, container) within the database.
 *
 * <p>Notes:
  <ul> <li>It is RECOMMENDED to capture the value as provided by the application without attempting
to do any case normalization. If the collection name is parsed from the query text, it SHOULD be the
first collection name found in the query and it SHOULD match the value provided in the query text
including any schema and database name prefix. For batch operations, if the individual operations
are known to have the same collection name then that collection name SHOULD be used, otherwise
{@code db.collection.name} SHOULD NOT be captured.</li> </ul>
 */
static constexpr const char *kDbCollectionName = "db.collection.name";

/**
 * The name of the database, fully qualified within the server address and port.
 *
 * <p>Notes:
  <ul> <li>If a database system has multiple namespace components, they SHOULD be concatenated
(potentially using database system specific conventions) from most general to most specific
namespace component, and more specific namespaces SHOULD NOT be captured without the more general
namespaces, to ensure that &quot;startswith&quot; queries for the more general namespaces will be
valid. Semantic conventions for individual database systems SHOULD document what {@code
db.namespace} means in the context of that system. It is RECOMMENDED to capture the value as
provided by the application without attempting to do any case normalization.</li> </ul>
 */
static constexpr const char *kDbNamespace = "db.namespace";

/**
 * The number of queries included in a <a
 href="/docs/database/database-spans.md#batch-operations">batch operation</a>.
 *
 * <p>Notes:
  <ul> <li>Operations are only considered batches when they contain two or more operations, and so
 {@code db.operation.batch.size} SHOULD never be {@code 1}.</li> </ul>
 */
static constexpr const char *kDbOperationBatchSize = "db.operation.batch.size";

/**
 * The name of the operation or command being executed.
 *
 * <p>Notes:
  <ul> <li>It is RECOMMENDED to capture the value as provided by the application without attempting
to do any case normalization. If the operation name is parsed from the query text, it SHOULD be the
first operation name found in the query. For batch operations, if the individual operations are
known to have the same operation name then that operation name SHOULD be used prepended by {@code
BATCH}, otherwise {@code db.operation.name} SHOULD be {@code BATCH} or some other database system
specific term if more applicable.</li> </ul>
 */
static constexpr const char *kDbOperationName = "db.operation.name";

/**
 * The database query being executed.
 *
 * <p>Notes:
  <ul> <li>For sanitization see <a
href="../../docs/database/database-spans.md#sanitization-of-dbquerytext">Sanitization of {@code
db.query.text}</a>. For batch operations, if the individual operations are known to have the same
query text then that query text SHOULD be used, otherwise all of the individual query texts SHOULD
be concatenated with separator {@code ;} or some other database system specific separator if more
applicable. Even though parameterized query text can potentially have sensitive data, by using a
parameterized query the user is giving a strong signal that any sensitive data will be passed as
parameter values, and the benefit to observability of capturing the static part of the query text by
default outweighs the risk.</li> </ul>
 */
static constexpr const char *kDbQueryText = "db.query.text";

/**
 * The database management system (DBMS) product as identified by the client instrumentation.
 *
 * <p>Notes:
  <ul> <li>The actual DBMS may differ from the one identified by the client. For example, when using
 PostgreSQL client libraries to connect to a CockroachDB, the {@code db.system} is set to {@code
 postgresql} based on the instrumentation's best knowledge.</li> </ul>
 */
static constexpr const char *kDbSystem = "db.system";

/**
 * The consistency level of the query. Based on consistency values from <a
 * href="https://docs.datastax.com/en/cassandra-oss/3.0/cassandra/dml/dmlConfigConsistency.html">CQL</a>.
 */
static constexpr const char *kDbCassandraConsistencyLevel = "db.cassandra.consistency_level";

/**
 * The data center of the coordinating node for a query.
 */
static constexpr const char *kDbCassandraCoordinatorDc = "db.cassandra.coordinator.dc";

/**
 * The ID of the coordinating node for a query.
 */
static constexpr const char *kDbCassandraCoordinatorId = "db.cassandra.coordinator.id";

/**
 * Whether or not the query is idempotent.
 */
static constexpr const char *kDbCassandraIdempotence = "db.cassandra.idempotence";

/**
 * The fetch size used for paging, i.e. how many rows will be returned at once.
 */
static constexpr const char *kDbCassandraPageSize = "db.cassandra.page_size";

/**
 * The number of times a query was speculatively executed. Not set or {@code 0} if the query was not
 * executed speculatively.
 */
static constexpr const char *kDbCassandraSpeculativeExecutionCount =
    "db.cassandra.speculative_execution_count";

/**
 * Unique Cosmos client instance id.
 */
static constexpr const char *kDbCosmosdbClientId = "db.cosmosdb.client_id";

/**
 * Cosmos client connection mode.
 */
static constexpr const char *kDbCosmosdbConnectionMode = "db.cosmosdb.connection_mode";

/**
 * CosmosDB Operation Type.
 */
static constexpr const char *kDbCosmosdbOperationType = "db.cosmosdb.operation_type";

/**
 * RU consumed for that operation
 */
static constexpr const char *kDbCosmosdbRequestCharge = "db.cosmosdb.request_charge";

/**
 * Request payload size in bytes
 */
static constexpr const char *kDbCosmosdbRequestContentLength = "db.cosmosdb.request_content_length";

/**
 * Cosmos DB status code.
 */
static constexpr const char *kDbCosmosdbStatusCode = "db.cosmosdb.status_code";

/**
 * Cosmos DB sub status code.
 */
static constexpr const char *kDbCosmosdbSubStatusCode = "db.cosmosdb.sub_status_code";

/**
 * Represents the human-readable identifier of the node/instance to which a request was routed.
 */
static constexpr const char *kDbElasticsearchNodeName = "db.elasticsearch.node.name";

/**
 * Name of the <a href="https://wikipedia.org/wiki/Deployment_environment">deployment
environment</a> (aka deployment tier).
 *
 * <p>Notes:
  <ul> <li>{@code deployment.environment.name} does not affect the uniqueness constraints defined
through the {@code service.namespace}, {@code service.name} and {@code service.instance.id} resource
attributes. This implies that resources carrying the following attribute combinations MUST be
considered to be identifying the same service:</li><li>{@code service.name=frontend}, {@code
deployment.environment.name=production}</li> <li>{@code service.name=frontend}, {@code
deployment.environment.name=staging}.</li>
 </ul>
 */
static constexpr const char *kDeploymentEnvironmentName = "deployment.environment.name";

/**
 * The id of the deployment.
 */
static constexpr const char *kDeploymentId = "deployment.id";

/**
 * The name of the deployment.
 */
static constexpr const char *kDeploymentName = "deployment.name";

/**
 * The status of the deployment.
 */
static constexpr const char *kDeploymentStatus = "deployment.status";

/**
 * Deprecated use the {@code device.app.lifecycle} event definition including {@code android.state}
 as a payload field instead.
 *
 * <p>Notes:
  <ul> <li>The Android lifecycle states are defined in <a
 href="https://developer.android.com/guide/components/activities/activity-lifecycle#lc">Activity
 lifecycle callbacks</a>, and from which the {@code OS identifiers} are derived.</li> </ul>
 */
static constexpr const char *kAndroidState = "android.state";

/**
 * Deprecated, use {@code cpu.mode} instead.
 *
 * @deprecated Deprecated, use `cpu.mode` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kContainerCpuState = "container.cpu.state";

/**
 * Deprecated, use {@code db.collection.name} instead.
 *
 * @deprecated Deprecated, use `db.collection.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbCassandraTable = "db.cassandra.table";

/**
 * Deprecated, use {@code server.address}, {@code server.port} attributes instead.
 *
 * @deprecated Deprecated, use `server.address`, `server.port` attributes instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbConnectionString = "db.connection_string";

/**
 * Deprecated, use {@code db.collection.name} instead.
 *
 * @deprecated Deprecated, use `db.collection.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbCosmosdbContainer = "db.cosmosdb.container";

/**
 * Deprecated, use {@code db.namespace} instead.
 *
 * @deprecated Deprecated, use `db.namespace` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbElasticsearchClusterName = "db.elasticsearch.cluster.name";

/**
 * Deprecated, no general replacement at this time. For Elasticsearch, use {@code
 * db.elasticsearch.node.name} instead.
 *
 * @deprecated Deprecated, no general replacement at this time. For Elasticsearch, use
 * `db.elasticsearch.node.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbInstanceId = "db.instance.id";

/**
 * Removed, no replacement at this time.
 *
 * @deprecated Removed, no replacement at this time.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbJdbcDriverClassname = "db.jdbc.driver_classname";

/**
 * Deprecated, use {@code db.collection.name} instead.
 *
 * @deprecated Deprecated, use `db.collection.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbMongodbCollection = "db.mongodb.collection";

/**
 * Deprecated, SQL Server instance is now populated as a part of {@code db.namespace} attribute.
 *
 * @deprecated Deprecated, SQL Server instance is now populated as a part of `db.namespace`
 * attribute.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbMssqlInstanceName = "db.mssql.instance_name";

/**
 * Deprecated, use {@code db.namespace} instead.
 *
 * @deprecated Deprecated, use `db.namespace` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbName = "db.name";

/**
 * Deprecated, use {@code db.operation.name} instead.
 *
 * @deprecated Deprecated, use `db.operation.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbOperation = "db.operation";

/**
 * Deprecated, use {@code db.namespace} instead.
 *
 * @deprecated Deprecated, use `db.namespace` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbRedisDatabaseIndex = "db.redis.database_index";

/**
 * Deprecated, use {@code db.collection.name} instead.
 *
 * @deprecated Deprecated, use `db.collection.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbSqlTable = "db.sql.table";

/**
 * The database statement being executed.
 *
 * @deprecated The database statement being executed.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbStatement = "db.statement";

/**
 * Deprecated, no replacement at this time.
 *
 * @deprecated Deprecated, no replacement at this time.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbUser = "db.user";

/**
 * Deprecated, use {@code db.client.connection.pool.name} instead.
 *
 * @deprecated Deprecated, use `db.client.connection.pool.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbClientConnectionsPoolName = "db.client.connections.pool.name";

/**
 * Deprecated, use {@code db.client.connection.state} instead.
 *
 * @deprecated Deprecated, use `db.client.connection.state` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDbClientConnectionsState = "db.client.connections.state";

/**
 * Deprecated, use {@code db.client.connection.pool.name} instead.
 *
 * @deprecated Deprecated, use `db.client.connection.pool.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kPoolName = "pool.name";

/**
 * Deprecated, use {@code db.client.connection.state} instead.
 *
 * @deprecated Deprecated, use `db.client.connection.state` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kState = "state";

/**
 * 'Deprecated, use {@code deployment.environment.name} instead.'
 *
 * @deprecated 'Deprecated, use `deployment.environment.name` instead.'.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kDeploymentEnvironment = "deployment.environment";

/**
 * Deprecated, use {@code user.id} instead.
 *
 * @deprecated Deprecated, use `user.id` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kEnduserId = "enduser.id";

/**
 * Deprecated, use {@code user.roles} instead.
 *
 * @deprecated Deprecated, use `user.roles` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kEnduserRole = "enduser.role";

/**
 * Deprecated, no replacement at this time.
 *
 * @deprecated Deprecated, no replacement at this time.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kEnduserScope = "enduser.scope";

/**
 * Deprecated, use {@code gen_ai.usage.output_tokens} instead.
 *
 * @deprecated Deprecated, use `gen_ai.usage.output_tokens` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kGenAiUsageCompletionTokens = "gen_ai.usage.completion_tokens";

/**
 * Deprecated, use {@code gen_ai.usage.input_tokens} instead.
 *
 * @deprecated Deprecated, use `gen_ai.usage.input_tokens` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kGenAiUsagePromptTokens = "gen_ai.usage.prompt_tokens";

/**
 * Deprecated, use {@code client.address} instead.
 *
 * @deprecated Deprecated, use `client.address` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpClientIp = "http.client_ip";

/**
 * Deprecated, use {@code network.protocol.name} instead.
 *
 * @deprecated Deprecated, use `network.protocol.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpFlavor = "http.flavor";

/**
 * Deprecated, use one of {@code server.address}, {@code client.address} or {@code
 * http.request.header.host} instead, depending on the usage.
 *
 * @deprecated Deprecated, use one of `server.address`, `client.address` or
 * `http.request.header.host` instead, depending on the usage.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpHost = "http.host";

/**
 * Deprecated, use {@code http.request.method} instead.
 *
 * @deprecated Deprecated, use `http.request.method` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpMethod = "http.method";

/**
 * Deprecated, use {@code http.request.header.content-length} instead.
 *
 * @deprecated Deprecated, use `http.request.header.content-length` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpRequestContentLength = "http.request_content_length";

/**
 * Deprecated, use {@code http.request.body.size} instead.
 *
 * @deprecated Deprecated, use `http.request.body.size` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpRequestContentLengthUncompressed =
    "http.request_content_length_uncompressed";

/**
 * Deprecated, use {@code http.response.header.content-length} instead.
 *
 * @deprecated Deprecated, use `http.response.header.content-length` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpResponseContentLength = "http.response_content_length";

/**
 * Deprecated, use {@code http.response.body.size} instead.
 *
 * @deprecated Deprecated, use `http.response.body.size` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpResponseContentLengthUncompressed =
    "http.response_content_length_uncompressed";

/**
 * Deprecated, use {@code url.scheme} instead.
 *
 * @deprecated Deprecated, use `url.scheme` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpScheme = "http.scheme";

/**
 * Deprecated, use {@code server.address} instead.
 *
 * @deprecated Deprecated, use `server.address` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpServerName = "http.server_name";

/**
 * Deprecated, use {@code http.response.status_code} instead.
 *
 * @deprecated Deprecated, use `http.response.status_code` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpStatusCode = "http.status_code";

/**
 * Deprecated, use {@code url.path} and {@code url.query} instead.
 *
 * @deprecated Deprecated, use `url.path` and `url.query` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpTarget = "http.target";

/**
 * Deprecated, use {@code url.full} instead.
 *
 * @deprecated Deprecated, use `url.full` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpUrl = "http.url";

/**
 * Deprecated, use {@code user_agent.original} instead.
 *
 * @deprecated Deprecated, use `user_agent.original` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kHttpUserAgent = "http.user_agent";

/**
 * Deprecated use the {@code device.app.lifecycle} event definition including {@code ios.state} as a
 payload field instead.
 *
 * <p>Notes:
  <ul> <li>The iOS lifecycle states are defined in the <a
 href="https://developer.apple.com/documentation/uikit/uiapplicationdelegate#1656902">UIApplicationDelegate
 documentation</a>, and from which the {@code OS terminology} column values are derived.</li> </ul>
 *
 * @deprecated Deprecated use the `device.app.lifecycle` event definition including `ios.state` as a
 payload field instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kIosState = "ios.state";

/**
 * Deprecated, no replacement at this time.
 *
 * @deprecated Deprecated, no replacement at this time.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessagingDestinationPublishAnonymous =
    "messaging.destination_publish.anonymous";

/**
 * Deprecated, no replacement at this time.
 *
 * @deprecated Deprecated, no replacement at this time.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessagingDestinationPublishName =
    "messaging.destination_publish.name";

/**
 * Deprecated, use {@code messaging.consumer.group.name} instead.
 *
 * @deprecated Deprecated, use `messaging.consumer.group.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessagingEventhubsConsumerGroup =
    "messaging.eventhubs.consumer.group";

/**
 * Deprecated, use {@code messaging.consumer.group.name} instead.
 *
 * @deprecated Deprecated, use `messaging.consumer.group.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessagingKafkaConsumerGroup = "messaging.kafka.consumer.group";

/**
 * Deprecated, use {@code messaging.destination.partition.id} instead.
 *
 * @deprecated Deprecated, use `messaging.destination.partition.id` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessagingKafkaDestinationPartition =
    "messaging.kafka.destination.partition";

/**
 * Deprecated, use {@code messaging.kafka.offset} instead.
 *
 * @deprecated Deprecated, use `messaging.kafka.offset` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessagingKafkaMessageOffset = "messaging.kafka.message.offset";

/**
 * Deprecated, use {@code messaging.operation.type} instead.
 *
 * @deprecated Deprecated, use `messaging.operation.type` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessagingOperation = "messaging.operation";

/**
 * Deprecated, use {@code messaging.consumer.group.name} instead.
 *
 * @deprecated Deprecated, use `messaging.consumer.group.name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessagingRocketmqClientGroup = "messaging.rocketmq.client_group";

/**
 * Deprecated, use {@code messaging.servicebus.destination.subscription_name} instead.
 *
 * @deprecated Deprecated, use `messaging.servicebus.destination.subscription_name` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessagingServicebusDestinationSubscriptionName =
    "messaging.servicebus.destination.subscription_name";

/**
 * Deprecated, use {@code network.local.address}.
 *
 * @deprecated Deprecated, use `network.local.address`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetHostIp = "net.host.ip";

/**
 * Deprecated, use {@code server.address}.
 *
 * @deprecated Deprecated, use `server.address`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetHostName = "net.host.name";

/**
 * Deprecated, use {@code server.port}.
 *
 * @deprecated Deprecated, use `server.port`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetHostPort = "net.host.port";

/**
 * Deprecated, use {@code network.peer.address}.
 *
 * @deprecated Deprecated, use `network.peer.address`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetPeerIp = "net.peer.ip";

/**
 * Deprecated, use {@code server.address} on client spans and {@code client.address} on server
 * spans.
 *
 * @deprecated Deprecated, use `server.address` on client spans and `client.address` on server
 * spans.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetPeerName = "net.peer.name";

/**
 * Deprecated, use {@code server.port} on client spans and {@code client.port} on server spans.
 *
 * @deprecated Deprecated, use `server.port` on client spans and `client.port` on server spans.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetPeerPort = "net.peer.port";

/**
 * Deprecated, use {@code network.protocol.name}.
 *
 * @deprecated Deprecated, use `network.protocol.name`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetProtocolName = "net.protocol.name";

/**
 * Deprecated, use {@code network.protocol.version}.
 *
 * @deprecated Deprecated, use `network.protocol.version`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetProtocolVersion = "net.protocol.version";

/**
 * Deprecated, use {@code network.transport} and {@code network.type}.
 *
 * @deprecated Deprecated, use `network.transport` and `network.type`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetSockFamily = "net.sock.family";

/**
 * Deprecated, use {@code network.local.address}.
 *
 * @deprecated Deprecated, use `network.local.address`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetSockHostAddr = "net.sock.host.addr";

/**
 * Deprecated, use {@code network.local.port}.
 *
 * @deprecated Deprecated, use `network.local.port`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetSockHostPort = "net.sock.host.port";

/**
 * Deprecated, use {@code network.peer.address}.
 *
 * @deprecated Deprecated, use `network.peer.address`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetSockPeerAddr = "net.sock.peer.addr";

/**
 * Deprecated, no replacement at this time.
 *
 * @deprecated Deprecated, no replacement at this time.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetSockPeerName = "net.sock.peer.name";

/**
 * Deprecated, use {@code network.peer.port}.
 *
 * @deprecated Deprecated, use `network.peer.port`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetSockPeerPort = "net.sock.peer.port";

/**
 * Deprecated, use {@code network.transport}.
 *
 * @deprecated Deprecated, use `network.transport`.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kNetTransport = "net.transport";

/**
 *
 *
 * @deprecated .
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kOtelLibraryName = "otel.library.name";

/**
 *
 *
 * @deprecated .
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kOtelLibraryVersion = "otel.library.version";

/**
 * Deprecated, use {@code cpu.mode} instead.
 *
 * @deprecated Deprecated, use `cpu.mode` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kProcessCpuState = "process.cpu.state";

/**
 * Deprecated, use {@code rpc.message.compressed_size} instead.
 *
 * @deprecated Deprecated, use `rpc.message.compressed_size` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessageCompressedSize = "message.compressed_size";

/**
 * Deprecated, use {@code rpc.message.id} instead.
 *
 * @deprecated Deprecated, use `rpc.message.id` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessageId = "message.id";

/**
 * Deprecated, use {@code rpc.message.type} instead.
 *
 * @deprecated Deprecated, use `rpc.message.type` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessageType = "message.type";

/**
 * Deprecated, use {@code rpc.message.uncompressed_size} instead.
 *
 * @deprecated Deprecated, use `rpc.message.uncompressed_size` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kMessageUncompressedSize = "message.uncompressed_size";

/**
 * Deprecated, use {@code cpu.mode} instead.
 *
 * @deprecated Deprecated, use `cpu.mode` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kSystemCpuState = "system.cpu.state";

/**
 * Deprecated, use {@code system.process.status} instead.
 *
 * @deprecated Deprecated, use `system.process.status` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kSystemProcessesStatus = "system.processes.status";

/**
 * Deprecated, use {@code server.address} instead.
 *
 * @deprecated Deprecated, use `server.address` instead.
 */
OPENTELEMETRY_DEPRECATED
static constexpr const char *kTlsClientServerName = "tls.client.server_name";

/**
 * Destination address - domain name if available without reverse DNS lookup; otherwise, IP address
 or Unix domain socket name.
 *
 * <p>Notes:
  <ul> <li>When observed from the source side, and when communicating through an intermediary,
 {@code destination.address} SHOULD represent the destination address behind any intermediaries, for
 example proxies, if it's available.</li> </ul>
 */
static constexpr const char *kDestinationAddress = "destination.address";

/**
 * Destination port number
 */
static constexpr const char *kDestinationPort = "destination.port";

/**
 * A unique identifier representing the device
 *
 * <p>Notes:
  <ul> <li>The device identifier MUST only be defined using the values outlined below. This value is
 not an advertising identifier and MUST NOT be used as such. On iOS (Swift or Objective-C), this
 value MUST be equal to the <a
 href="https://developer.apple.com/documentation/uikit/uidevice/1620059-identifierforvendor">vendor
 identifier</a>. On Android (Java or Kotlin), this value MUST be equal to the Firebase Installation
 ID or a globally unique UUID which is persisted across sessions in your application. More
 information can be found <a
 href="https://developer.android.com/training/articles/user-data-ids">here</a> on best practices and
 exact implementation details. Caution should be taken when storing personal data or anything which
 can identify a user. GDPR and data protection laws may apply, ensure you do your own due
 diligence.</li> </ul>
 */
static constexpr const char *kDeviceId = "device.id";

/**
 * The name of the device manufacturer
 *
 * <p>Notes:
  <ul> <li>The Android OS provides this field via <a
 href="https://developer.android.com/reference/android/os/Build#MANUFACTURER">Build</a>. iOS apps
 SHOULD hardcode the value {@code Apple}.</li> </ul>
 */
static constexpr const char *kDeviceManufacturer = "device.manufacturer";

/**
 * The model identifier for the device
 *
 * <p>Notes:
  <ul> <li>It's recommended this value represents a machine-readable version of the model identifier
 rather than the market or consumer-friendly name of the device.</li> </ul>
 */
static constexpr const char *kDeviceModelIdentifier = "device.model.identifier";

/**
 * The marketing name for the device model
 *
 * <p>Notes:
  <ul> <li>It's recommended this value represents a human-readable version of the device model
 rather than a machine-readable alternative.</li> </ul>
 */
static constexpr const char *kDeviceModelName = "device.model.name";

/**
 * The disk IO operation direction.
 */
static constexpr const char *kDiskIoDirection = "disk.io.direction";

/**
 * The name being queried.
 *
 * <p>Notes:
  <ul> <li>If the name field contains non-printable characters (below 32 or above 126), those
 characters should be represented as escaped base 10 integers (\DDD). Back slashes and quotes should
 be escaped. Tabs, carriage returns, and line feeds should be converted to \t, \r, and \n
 respectively.</li> </ul>
 */
static constexpr const char *kDnsQuestionName = "dns.question.name";

/**
 * Describes a class of error the operation ended with.
 *
 * <p>Notes:
  <ul> <li>The {@code error.type} SHOULD be predictable, and SHOULD have low
cardinality.</li><li>When {@code error.type} is set to a type (e.g., an exception type), its
canonical class name identifying the type within the artifact SHOULD be
used.</li><li>Instrumentations SHOULD document the list of errors they report.</li><li>The
cardinality of {@code error.type} within one instrumentation library SHOULD be low. Telemetry
consumers that aggregate data from multiple instrumentation libraries and applications should be
prepared for {@code error.type} to have high cardinality at query time when no additional filters
are applied.</li><li>If the operation has completed successfully, instrumentations SHOULD NOT set
{@code error.type}.</li><li>If a specific domain defines its own set of error identifiers (such as
HTTP or gRPC status codes), it's RECOMMENDED to:</li><li>Use a domain-specific attribute</li>
<li>Set {@code error.type} to capture all errors, regardless of whether they are defined within the
domain-specific set or not.</li>
 </ul>
 */
static constexpr const char *kErrorType = "error.type";

/**
 * Identifies the class / type of event.
 *
 * <p>Notes:
  <ul> <li>Event names are subject to the same rules as <a
 href="/docs/general/attribute-naming.md">attribute names</a>. Notably, event names are namespaced
 to avoid collisions and provide a clean separation of semantics for events in separate domains like
 browser, mobile, and kubernetes.</li> </ul>
 */
static constexpr const char *kEventName = "event.name";

/**
 * SHOULD be set to true if the exception event is recorded at a point where it is known that the
exception is escaping the scope of the span.
 *
 * <p>Notes:
  <ul> <li>An exception is considered to have escaped (or left) the scope of a span,
if that span is ended while the exception is still logically &quot;in flight&quot;.
This may be actually &quot;in flight&quot; in some languages (e.g. if the exception
is passed to a Context manager's {@code __exit__} method in Python) but will
usually be caught at the point of recording the exception in most languages.</li><li>It is usually
not possible to determine at the point where an exception is thrown whether it will escape the scope
of a span. However, it is trivial to know that an exception will escape, if one checks for an active
exception just before ending the span, as done in the <a
href="https://opentelemetry.io/docs/specs/semconv/exceptions/exceptions-spans/#recording-an-exception">example
for recording span exceptions</a>.</li><li>It follows that an exception may still escape the scope
of the span even if the {@code exception.escaped} attribute was not set or set to false, since the
event might have been recorded at a time where it was not clear whether the exception will
escape.</li> </ul>
 */
static constexpr const char *kExceptionEscaped = "exception.escaped";

/**
 * The exception message.
 */
static constexpr const char *kExceptionMessage = "exception.message";

/**
 * A stacktrace as a string in the natural representation for the language runtime. The
 * representation is to be determined and documented by each language SIG.
 */
static constexpr const char *kExceptionStacktrace = "exception.stacktrace";

/**
 * The type of the exception (its fully-qualified class name, if applicable). The dynamic type of
 * the exception should be preferred over the static type in languages that support it.
 */
static constexpr const char *kExceptionType = "exception.type";

/**
 * A boolean that is true if the serverless function is executed for the first time (aka
 * cold-start).
 */
static constexpr const char *kFaasColdstart = "faas.coldstart";

/**
 * A string containing the schedule period as <a
 * href="https://docs.oracle.com/cd/E12058_01/doc/doc.1014/e12030/cron_expressions.htm">Cron
 * Expression</a>.
 */
static constexpr const char *kFaasCron = "faas.cron";

/**
 * The name of the source on which the triggering operation was performed. For example, in Cloud
 * Storage or S3 corresponds to the bucket name, and in Cosmos DB to the database name.
 */
static constexpr const char *kFaasDocumentCollection = "faas.document.collection";

/**
 * The document name/table subjected to the operation. For example, in Cloud Storage or S3 is the
 * name of the file, and in Cosmos DB the table name.
 */
static constexpr const char *kFaasDocumentName = "faas.document.name";

/**
 * Describes the type of the operation that was performed on the data.
 */
static constexpr const char *kFaasDocumentOperation = "faas.document.operation";

/**
 * A string containing the time when the data was accessed in the <a
 * href="https://www.iso.org/iso-8601-date-and-time-format.html">ISO 8601</a> format expressed in <a
 * href="https://www.w3.org/TR/NOTE-datetime">UTC</a>.
 */
static constexpr const char *kFaasDocumentTime = "faas.document.time";

/**
 * The execution environment ID as a string, that will be potentially reused for other invocations
 to the same function/function version.
 *
 * <p>Notes:
  <ul> <li><strong>AWS Lambda:</strong> Use the (full) log stream name.</li>
 </ul>
 */
static constexpr const char *kFaasInstance = "faas.instance";

/**
 * The invocation ID of the current function invocation.
 */
static constexpr const char *kFaasInvocationId = "faas.invocation_id";

/**
 * The name of the invoked function.
 *
 * <p>Notes:
  <ul> <li>SHOULD be equal to the {@code faas.name} resource attribute of the invoked function.</li>
 </ul>
 */
static constexpr const char *kFaasInvokedName = "faas.invoked_name";

/**
 * The cloud provider of the invoked function.
 *
 * <p>Notes:
  <ul> <li>SHOULD be equal to the {@code cloud.provider} resource attribute of the invoked
 function.</li> </ul>
 */
static constexpr const char *kFaasInvokedProvider = "faas.invoked_provider";

/**
 * The cloud region of the invoked function.
 *
 * <p>Notes:
  <ul> <li>SHOULD be equal to the {@code cloud.region} resource attribute of the invoked
 function.</li> </ul>
 */
static constexpr const char *kFaasInvokedRegion = "faas.invoked_region";

/**
 * The amount of memory available to the serverless function converted to Bytes.
 *
 * <p>Notes:
  <ul> <li>It's recommended to set this attribute since e.g. too little memory can easily stop a
 Java AWS Lambda function from working correctly. On AWS Lambda, the environment variable {@code
 AWS_LAMBDA_FUNCTION_MEMORY_SIZE} provides this information (which must be multiplied by
 1,048,576).</li> </ul>
 */
static constexpr const char *kFaasMaxMemory = "faas.max_memory";

/**
 * The name of the single function that this runtime instance executes.
 *
 * <p>Notes:
  <ul> <li>This is the name of the function as configured/deployed on the FaaS
platform and is usually different from the name of the callback
function (which may be stored in the
<a href="/docs/general/attributes.md#source-code-attributes">{@code code.namespace}/{@code
code.function}</a> span attributes).</li><li>For some cloud providers, the above definition is
ambiguous. The following definition of function name MUST be used for this attribute (and
consequently the span name) for the listed cloud providers/products:</li><li><strong>Azure:</strong>
The full name {@code <FUNCAPP>/<FUNC>}, i.e., function app name followed by a forward slash followed
by the function name (this form can also be seen in the resource JSON for the function). This means
that a span attribute MUST be used, as an Azure function app can host multiple functions that would
usually share a TracerProvider (see also the {@code cloud.resource_id} attribute).</li>
 </ul>
 */
static constexpr const char *kFaasName = "faas.name";

/**
 * A string containing the function invocation time in the <a
 * href="https://www.iso.org/iso-8601-date-and-time-format.html">ISO 8601</a> format expressed in <a
 * href="https://www.w3.org/TR/NOTE-datetime">UTC</a>.
 */
static constexpr const char *kFaasTime = "faas.time";

/**
 * Type of the trigger which caused this function invocation.
 */
static constexpr const char *kFaasTrigger = "faas.trigger";

/**
 * The immutable version of the function being executed.
 *
 * <p>Notes:
  <ul> <li>Depending on the cloud provider and platform, use:</li><li><strong>AWS Lambda:</strong>
The <a href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-versions.html">function
version</a> (an integer represented as a decimal string).</li> <li><strong>Google Cloud Run
(Services):</strong> The <a href="https://cloud.google.com/run/docs/managing/revisions">revision</a>
(i.e., the function name plus the revision suffix).</li>
<li><strong>Google Cloud Functions:</strong> The value of the
<a
href="https://cloud.google.com/functions/docs/env-var#runtime_environment_variables_set_automatically">{@code
K_REVISION} environment variable</a>.</li> <li><strong>Azure Functions:</strong> Not applicable. Do
not set this attribute.</li>
 </ul>
 */
static constexpr const char *kFaasVersion = "faas.version";

/**
 * The unique identifier of the feature flag.
 */
static constexpr const char *kFeatureFlagKey = "feature_flag.key";

/**
 * The name of the service provider that performs the flag evaluation.
 */
static constexpr const char *kFeatureFlagProviderName = "feature_flag.provider_name";

/**
 * SHOULD be a semantic identifier for a value. If one is unavailable, a stringified version of the
value can be used.
 *
 * <p>Notes:
  <ul> <li>A semantic identifier, commonly referred to as a variant, provides a means
for referring to a value without including the value itself. This can
provide additional context for understanding the meaning behind a value.
For example, the variant {@code red} maybe be used for the value {@code #c05543}.</li><li>A
stringified version of the value can be used in situations where a semantic identifier is
unavailable. String representation of the value should be determined by the implementer.</li> </ul>
 */
static constexpr const char *kFeatureFlagVariant = "feature_flag.variant";

/**
 * Directory where the file is located. It should include the drive letter, when appropriate.
 */
static constexpr const char *kFileDirectory = "file.directory";

/**
 * File extension, excluding the leading dot.
 *
 * <p>Notes:
  <ul> <li>When the file name has multiple extensions (example.tar.gz), only the last one should be
 captured (&quot;gz&quot;, not &quot;tar.gz&quot;).</li> </ul>
 */
static constexpr const char *kFileExtension = "file.extension";

/**
 * Name of the file including the extension, without the directory.
 */
static constexpr const char *kFileName = "file.name";

/**
 * Full path to the file, including the file name. It should include the drive letter, when
 * appropriate.
 */
static constexpr const char *kFilePath = "file.path";

/**
 * File size in bytes.
 */
static constexpr const char *kFileSize = "file.size";

/**
 * Identifies the Google Cloud service for which the official client library is intended.
 *
 * <p>Notes:
  <ul> <li>Intended to be a stable identifier for Google Cloud client libraries that is uniform
 across implementation languages. The value should be derived from the canonical service domain for
 the service; for example, 'foo.googleapis.com' should result in a value of 'foo'.</li> </ul>
 */
static constexpr const char *kGcpClientService = "gcp.client.service";

/**
 * The name of the Cloud Run <a
 * href="https://cloud.google.com/run/docs/managing/job-executions">execution</a> being run for the
 * Job, as set by the <a
 * href="https://cloud.google.com/run/docs/container-contract#jobs-env-vars">{@code
 * CLOUD_RUN_EXECUTION}</a> environment variable.
 */
static constexpr const char *kGcpCloudRunJobExecution = "gcp.cloud_run.job.execution";

/**
 * The index for a task within an execution as provided by the <a
 * href="https://cloud.google.com/run/docs/container-contract#jobs-env-vars">{@code
 * CLOUD_RUN_TASK_INDEX}</a> environment variable.
 */
static constexpr const char *kGcpCloudRunJobTaskIndex = "gcp.cloud_run.job.task_index";

/**
 * The hostname of a GCE instance. This is the full value of the default or <a
 * href="https://cloud.google.com/compute/docs/instances/custom-hostname-vm">custom hostname</a>.
 */
static constexpr const char *kGcpGceInstanceHostname = "gcp.gce.instance.hostname";

/**
 * The instance name of a GCE instance. This is the value provided by {@code host.name}, the visible
 * name of the instance in the Cloud Console UI, and the prefix for the default hostname of the
 * instance as defined by the <a
 * href="https://cloud.google.com/compute/docs/internal-dns#instance-fully-qualified-domain-names">default
 * internal DNS name</a>.
 */
static constexpr const char *kGcpGceInstanceName = "gcp.gce.instance.name";

/**
 * The full response received from the GenAI model.
 *
 * <p>Notes:
  <ul> <li>It's RECOMMENDED to format completions as JSON string matching <a
 href="https://platform.openai.com/docs/guides/text-generation">OpenAI messages format</a></li>
 </ul>
 */
static constexpr const char *kGenAiCompletion = "gen_ai.completion";

/**
 * The name of the operation being performed.
 *
 * <p>Notes:
  <ul> <li>If one of the predefined values applies, but specific system uses a different name it's
 RECOMMENDED to document it in the semantic conventions for specific GenAI system and use
 system-specific name in the instrumentation. If a different name is not documented, instrumentation
 libraries SHOULD use applicable predefined value.</li> </ul>
 */
static constexpr const char *kGenAiOperationName = "gen_ai.operation.name";

/**
 * The full prompt sent to the GenAI model.
 *
 * <p>Notes:
  <ul> <li>It's RECOMMENDED to format prompts as JSON string matching <a
 href="https://platform.openai.com/docs/guides/text-generation">OpenAI messages format</a></li>
 </ul>
 */
static constexpr const char *kGenAiPrompt = "gen_ai.prompt";

/**
 * The frequency penalty setting for the GenAI request.
 */
static constexpr const char *kGenAiRequestFrequencyPenalty = "gen_ai.request.frequency_penalty";

/**
 * The maximum number of tokens the model generates for a request.
 */
static constexpr const char *kGenAiRequestMaxTokens = "gen_ai.request.max_tokens";

/**
 * The name of the GenAI model a request is being made to.
 */
static constexpr const char *kGenAiRequestModel = "gen_ai.request.model";

/**
 * The presence penalty setting for the GenAI request.
 */
static constexpr const char *kGenAiRequestPresencePenalty = "gen_ai.request.presence_penalty";

/**
 * List of sequences that the model will use to stop generating further tokens.
 */
static constexpr const char *kGenAiRequestStopSequences = "gen_ai.request.stop_sequences";

/**
 * The temperature setting for the GenAI request.
 */
static constexpr const char *kGenAiRequestTemperature = "gen_ai.request.temperature";

/**
 * The top_k sampling setting for the GenAI request.
 */
static constexpr const char *kGenAiRequestTopK = "gen_ai.request.top_k";

/**
 * The top_p sampling setting for the GenAI request.
 */
static constexpr const char *kGenAiRequestTopP = "gen_ai.request.top_p";

/**
 * Array of reasons the model stopped generating tokens, corresponding to each generation received.
 */
static constexpr const char *kGenAiResponseFinishReasons = "gen_ai.response.finish_reasons";

/**
 * The unique identifier for the completion.
 */
static constexpr const char *kGenAiResponseId = "gen_ai.response.id";

/**
 * The name of the model that generated the response.
 */
static constexpr const char *kGenAiResponseModel = "gen_ai.response.model";

/**
 * The Generative AI product as identified by the client or server instrumentation.
 *
 * <p>Notes:
  <ul> <li>The {@code gen_ai.system} describes a family of GenAI models with specific model
identified by {@code gen_ai.request.model} and {@code gen_ai.response.model} attributes.</li><li>The
actual GenAI product may differ from the one identified by the client. For example, when using
OpenAI client libraries to communicate with Mistral, the {@code gen_ai.system} is set to {@code
openai} based on the instrumentation's best knowledge.</li><li>For custom model, a custom friendly
name SHOULD be used. If none of these options apply, the {@code gen_ai.system} SHOULD be set to
{@code _OTHER}.</li> </ul>
 */
static constexpr const char *kGenAiSystem = "gen_ai.system";

/**
 * The type of token being counted.
 */
static constexpr const char *kGenAiTokenType = "gen_ai.token.type";

/**
 * The number of tokens used in the GenAI input (prompt).
 */
static constexpr const char *kGenAiUsageInputTokens = "gen_ai.usage.input_tokens";

/**
 * The number of tokens used in the GenAI response (completion).
 */
static constexpr const char *kGenAiUsageOutputTokens = "gen_ai.usage.output_tokens";

/**
 * The type of memory.
 */
static constexpr const char *kGoMemoryType = "go.memory.type";

/**
 * The GraphQL document being executed.
 *
 * <p>Notes:
  <ul> <li>The value may be sanitized to exclude sensitive information.</li> </ul>
 */
static constexpr const char *kGraphqlDocument = "graphql.document";

/**
 * The name of the operation being executed.
 */
static constexpr const char *kGraphqlOperationName = "graphql.operation.name";

/**
 * The type of the operation being executed.
 */
static constexpr const char *kGraphqlOperationType = "graphql.operation.type";

/**
 * Unique identifier for the application
 */
static constexpr const char *kHerokuAppId = "heroku.app.id";

/**
 * Commit hash for the current release
 */
static constexpr const char *kHerokuReleaseCommit = "heroku.release.commit";

/**
 * Time and date the release was created
 */
static constexpr const char *kHerokuReleaseCreationTimestamp = "heroku.release.creation_timestamp";

/**
 * The CPU architecture the host system is running on.
 */
static constexpr const char *kHostArch = "host.arch";

/**
 * The amount of level 2 memory cache available to the processor (in Bytes).
 */
static constexpr const char *kHostCpuCacheL2Size = "host.cpu.cache.l2.size";

/**
 * Family or generation of the CPU.
 */
static constexpr const char *kHostCpuFamily = "host.cpu.family";

/**
 * Model identifier. It provides more granular information about the CPU, distinguishing it from
 * other CPUs within the same family.
 */
static constexpr const char *kHostCpuModelId = "host.cpu.model.id";

/**
 * Model designation of the processor.
 */
static constexpr const char *kHostCpuModelName = "host.cpu.model.name";

/**
 * Stepping or core revisions.
 */
static constexpr const char *kHostCpuStepping = "host.cpu.stepping";

/**
 * Processor manufacturer identifier. A maximum 12-character string.
 *
 * <p>Notes:
  <ul> <li><a href="https://wiki.osdev.org/CPUID">CPUID</a> command returns the vendor ID string in
 EBX, EDX and ECX registers. Writing these to memory in this order results in a 12-character
 string.</li> </ul>
 */
static constexpr const char *kHostCpuVendorId = "host.cpu.vendor.id";

/**
 * Unique host ID. For Cloud, this must be the instance_id assigned by the cloud provider. For
 * non-containerized systems, this should be the {@code machine-id}. See the table below for the
 * sources to use to determine the {@code machine-id} based on operating system.
 */
static constexpr const char *kHostId = "host.id";

/**
 * VM image ID or host OS image ID. For Cloud, this value is from the provider.
 */
static constexpr const char *kHostImageId = "host.image.id";

/**
 * Name of the VM image or OS install the host was instantiated from.
 */
static constexpr const char *kHostImageName = "host.image.name";

/**
 * The version string of the VM image or host OS as defined in <a
 * href="/docs/resource/README.md#version-attributes">Version Attributes</a>.
 */
static constexpr const char *kHostImageVersion = "host.image.version";

/**
 * Available IP addresses of the host, excluding loopback interfaces.
 *
 * <p>Notes:
  <ul> <li>IPv4 Addresses MUST be specified in dotted-quad notation. IPv6 addresses MUST be
 specified in the <a href="https://www.rfc-editor.org/rfc/rfc5952.html">RFC 5952</a> format.</li>
 </ul>
 */
static constexpr const char *kHostIp = "host.ip";

/**
 * Available MAC addresses of the host, excluding loopback interfaces.
 *
 * <p>Notes:
  <ul> <li>MAC Addresses MUST be represented in <a
 href="https://standards.ieee.org/wp-content/uploads/import/documents/tutorials/eui.pdf">IEEE RA
 hexadecimal form</a>: as hyphen-separated octets in uppercase hexadecimal form from most to least
 significant.</li> </ul>
 */
static constexpr const char *kHostMac = "host.mac";

/**
 * Name of the host. On Unix systems, it may contain what the hostname command returns, or the fully
 * qualified hostname, or another name specified by the user.
 */
static constexpr const char *kHostName = "host.name";

/**
 * Type of host. For Cloud, this must be the machine type.
 */
static constexpr const char *kHostType = "host.type";

/**
 * State of the HTTP connection in the HTTP connection pool.
 */
static constexpr const char *kHttpConnectionState = "http.connection.state";

/**
 * The size of the request payload body in bytes. This is the number of bytes transferred excluding
 * headers and is often, but not always, present as the <a
 * href="https://www.rfc-editor.org/rfc/rfc9110.html#field.content-length">Content-Length</a>
 * header. For requests using transport encoding, this should be the compressed size.
 */
static constexpr const char *kHttpRequestBodySize = "http.request.body.size";

/**
 * HTTP request method.
 *
 * <p>Notes:
  <ul> <li>HTTP request method value SHOULD be &quot;known&quot; to the instrumentation.
By default, this convention defines &quot;known&quot; methods as the ones listed in <a
href="https://www.rfc-editor.org/rfc/rfc9110.html#name-methods">RFC9110</a> and the PATCH method
defined in <a href="https://www.rfc-editor.org/rfc/rfc5789.html">RFC5789</a>.</li><li>If the HTTP
request method is not known to instrumentation, it MUST set the {@code http.request.method}
attribute to {@code _OTHER}.</li><li>If the HTTP instrumentation could end up converting valid HTTP
request methods to {@code _OTHER}, then it MUST provide a way to override the list of known HTTP
methods. If this override is done via environment variable, then the environment variable MUST be
named OTEL_INSTRUMENTATION_HTTP_KNOWN_METHODS and support a comma-separated list of case-sensitive
known HTTP methods (this list MUST be a full override of the default known method, it is not a list
of known methods in addition to the defaults).</li><li>HTTP method names are case-sensitive and
{@code http.request.method} attribute value MUST match a known HTTP method name exactly.
Instrumentations for specific web frameworks that consider HTTP methods to be case insensitive,
SHOULD populate a canonical equivalent. Tracing instrumentations that do so, MUST also set {@code
http.request.method_original} to the original value.</li> </ul>
 */
static constexpr const char *kHttpRequestMethod = "http.request.method";

/**
 * Original HTTP method sent by the client in the request line.
 */
static constexpr const char *kHttpRequestMethodOriginal = "http.request.method_original";

/**
 * The ordinal number of request resending attempt (for any reason, including redirects).
 *
 * <p>Notes:
  <ul> <li>The resend count SHOULD be updated each time an HTTP request gets resent by the client,
 regardless of what was the cause of the resending (e.g. redirection, authorization failure, 503
 Server Unavailable, network issues, or any other).</li> </ul>
 */
static constexpr const char *kHttpRequestResendCount = "http.request.resend_count";

/**
 * The total size of the request in bytes. This should be the total number of bytes sent over the
 * wire, including the request line (HTTP/1.1), framing (HTTP/2 and HTTP/3), headers, and request
 * body if any.
 */
static constexpr const char *kHttpRequestSize = "http.request.size";

/**
 * The size of the response payload body in bytes. This is the number of bytes transferred excluding
 * headers and is often, but not always, present as the <a
 * href="https://www.rfc-editor.org/rfc/rfc9110.html#field.content-length">Content-Length</a>
 * header. For requests using transport encoding, this should be the compressed size.
 */
static constexpr const char *kHttpResponseBodySize = "http.response.body.size";

/**
 * The total size of the response in bytes. This should be the total number of bytes sent over the
 * wire, including the status line (HTTP/1.1), framing (HTTP/2 and HTTP/3), headers, and response
 * body and trailers if any.
 */
static constexpr const char *kHttpResponseSize = "http.response.size";

/**
 * <a href="https://tools.ietf.org/html/rfc7231#section-6">HTTP response status code</a>.
 */
static constexpr const char *kHttpResponseStatusCode = "http.response.status_code";

/**
 * The matched route, that is, the path template in the format used by the respective server
framework.
 *
 * <p>Notes:
  <ul> <li>MUST NOT be populated when this is not supported by the HTTP server framework as the
route attribute should have low-cardinality and the URI path can NOT substitute it. SHOULD include
the <a href="/docs/http/http-spans.md#http-server-definitions">application root</a> if there is
one.</li> </ul>
 */
static constexpr const char *kHttpRoute = "http.route";

/**
 * Name of the buffer pool.
 *
 * <p>Notes:
  <ul> <li>Pool names are generally obtained via <a
 href="https://docs.oracle.com/en/java/javase/11/docs/api/java.management/java/lang/management/BufferPoolMXBean.html#getName()">BufferPoolMXBean#getName()</a>.</li>
 </ul>
 */
static constexpr const char *kJvmBufferPoolName = "jvm.buffer.pool.name";

/**
 * Name of the garbage collector action.
 *
 * <p>Notes:
  <ul> <li>Garbage collector action is generally obtained via <a
 href="https://docs.oracle.com/en/java/javase/11/docs/api/jdk.management/com/sun/management/GarbageCollectionNotificationInfo.html#getGcAction()">GarbageCollectionNotificationInfo#getGcAction()</a>.</li>
 </ul>
 */
static constexpr const char *kJvmGcAction = "jvm.gc.action";

/**
 * Name of the garbage collector.
 *
 * <p>Notes:
  <ul> <li>Garbage collector name is generally obtained via <a
 href="https://docs.oracle.com/en/java/javase/11/docs/api/jdk.management/com/sun/management/GarbageCollectionNotificationInfo.html#getGcName()">GarbageCollectionNotificationInfo#getGcName()</a>.</li>
 </ul>
 */
static constexpr const char *kJvmGcName = "jvm.gc.name";

/**
 * Name of the memory pool.
 *
 * <p>Notes:
  <ul> <li>Pool names are generally obtained via <a
 href="https://docs.oracle.com/en/java/javase/11/docs/api/java.management/java/lang/management/MemoryPoolMXBean.html#getName()">MemoryPoolMXBean#getName()</a>.</li>
 </ul>
 */
static constexpr const char *kJvmMemoryPoolName = "jvm.memory.pool.name";

/**
 * The type of memory.
 */
static constexpr const char *kJvmMemoryType = "jvm.memory.type";

/**
 * Whether the thread is daemon or not.
 */
static constexpr const char *kJvmThreadDaemon = "jvm.thread.daemon";

/**
 * State of the thread.
 */
static constexpr const char *kJvmThreadState = "jvm.thread.state";

/**
 * The name of the cluster.
 */
static constexpr const char *kK8sClusterName = "k8s.cluster.name";

/**
 * A pseudo-ID for the cluster, set to the UID of the {@code kube-system} namespace.
 *
 * <p>Notes:
  <ul> <li>K8s doesn't have support for obtaining a cluster ID. If this is ever
added, we will recommend collecting the {@code k8s.cluster.uid} through the
official APIs. In the meantime, we are able to use the {@code uid} of the
{@code kube-system} namespace as a proxy for cluster ID. Read on for the
rationale.</li><li>Every object created in a K8s cluster is assigned a distinct UID. The
{@code kube-system} namespace is used by Kubernetes itself and will exist
for the lifetime of the cluster. Using the {@code uid} of the {@code kube-system}
namespace is a reasonable proxy for the K8s ClusterID as it will only
change if the cluster is rebuilt. Furthermore, Kubernetes UIDs are
UUIDs as standardized by
<a href="https://www.itu.int/ITU-T/studygroups/com17/oid.html">ISO/IEC 9834-8 and ITU-T X.667</a>.
Which states:</li><blockquote>
<li>If generated according to one of the mechanisms defined in Rec.</li></blockquote>
<li>ITU-T X.667 | ISO/IEC 9834-8, a UUID is either guaranteed to be
  different from all other UUIDs generated before 3603 A.D., or is
  extremely likely to be different (depending on the mechanism chosen).</li><li>Therefore, UIDs
between clusters should be extremely unlikely to conflict.</li> </ul>
 */
static constexpr const char *kK8sClusterUid = "k8s.cluster.uid";

/**
 * The name of the Container from Pod specification, must be unique within a Pod. Container runtime
 * usually uses different globally unique name ({@code container.name}).
 */
static constexpr const char *kK8sContainerName = "k8s.container.name";

/**
 * Number of times the container was restarted. This attribute can be used to identify a particular
 * container (running or stopped) within a container spec.
 */
static constexpr const char *kK8sContainerRestartCount = "k8s.container.restart_count";

/**
 * Last terminated reason of the Container.
 */
static constexpr const char *kK8sContainerStatusLastTerminatedReason =
    "k8s.container.status.last_terminated_reason";

/**
 * The name of the CronJob.
 */
static constexpr const char *kK8sCronjobName = "k8s.cronjob.name";

/**
 * The UID of the CronJob.
 */
static constexpr const char *kK8sCronjobUid = "k8s.cronjob.uid";

/**
 * The name of the DaemonSet.
 */
static constexpr const char *kK8sDaemonsetName = "k8s.daemonset.name";

/**
 * The UID of the DaemonSet.
 */
static constexpr const char *kK8sDaemonsetUid = "k8s.daemonset.uid";

/**
 * The name of the Deployment.
 */
static constexpr const char *kK8sDeploymentName = "k8s.deployment.name";

/**
 * The UID of the Deployment.
 */
static constexpr const char *kK8sDeploymentUid = "k8s.deployment.uid";

/**
 * The name of the Job.
 */
static constexpr const char *kK8sJobName = "k8s.job.name";

/**
 * The UID of the Job.
 */
static constexpr const char *kK8sJobUid = "k8s.job.uid";

/**
 * The name of the namespace that the pod is running in.
 */
static constexpr const char *kK8sNamespaceName = "k8s.namespace.name";

/**
 * The name of the Node.
 */
static constexpr const char *kK8sNodeName = "k8s.node.name";

/**
 * The UID of the Node.
 */
static constexpr const char *kK8sNodeUid = "k8s.node.uid";

/**
 * The name of the Pod.
 */
static constexpr const char *kK8sPodName = "k8s.pod.name";

/**
 * The UID of the Pod.
 */
static constexpr const char *kK8sPodUid = "k8s.pod.uid";

/**
 * The name of the ReplicaSet.
 */
static constexpr const char *kK8sReplicasetName = "k8s.replicaset.name";

/**
 * The UID of the ReplicaSet.
 */
static constexpr const char *kK8sReplicasetUid = "k8s.replicaset.uid";

/**
 * The name of the StatefulSet.
 */
static constexpr const char *kK8sStatefulsetName = "k8s.statefulset.name";

/**
 * The UID of the StatefulSet.
 */
static constexpr const char *kK8sStatefulsetUid = "k8s.statefulset.uid";

/**
 * The Linux Slab memory state
 */
static constexpr const char *kLinuxMemorySlabState = "linux.memory.slab.state";

/**
 * The stream associated with the log. See below for a list of well-known values.
 */
static constexpr const char *kLogIostream = "log.iostream";

/**
 * The basename of the file.
 */
static constexpr const char *kLogFileName = "log.file.name";

/**
 * The basename of the file, with symlinks resolved.
 */
static constexpr const char *kLogFileNameResolved = "log.file.name_resolved";

/**
 * The full path to the file.
 */
static constexpr const char *kLogFilePath = "log.file.path";

/**
 * The full path to the file, with symlinks resolved.
 */
static constexpr const char *kLogFilePathResolved = "log.file.path_resolved";

/**
 * The complete orignal Log Record.
 *
 * <p>Notes:
  <ul> <li>This value MAY be added when processing a Log Record which was originally transmitted as
 a string or equivalent data type AND the Body field of the Log Record does not contain the same
 value. (e.g. a syslog or a log record read from a file.)</li> </ul>
 */
static constexpr const char *kLogRecordOriginal = "log.record.original";

/**
 * A unique identifier for the Log Record.
 *
 * <p>Notes:
  <ul> <li>If an id is provided, other log records with the same id will be considered duplicates
and can be removed safely. This means, that two distinguishable log records MUST have different
values. The id MAY be an <a href="https://github.com/ulid/spec">Universally Unique Lexicographically
Sortable Identifier (ULID)</a>, but other identifiers (e.g. UUID) may be used as needed.</li> </ul>
 */
static constexpr const char *kLogRecordUid = "log.record.uid";

/**
 * The number of messages sent, received, or processed in the scope of the batching operation.
 *
 * <p>Notes:
  <ul> <li>Instrumentations SHOULD NOT set {@code messaging.batch.message_count} on spans that
 operate with a single message. When a messaging client library supports both batch and
 single-message API for the same operation, instrumentations SHOULD use {@code
 messaging.batch.message_count} for batching APIs and SHOULD NOT use it for single-message
 APIs.</li> </ul>
 */
static constexpr const char *kMessagingBatchMessageCount = "messaging.batch.message_count";

/**
 * A unique identifier for the client that consumes or produces a message.
 */
static constexpr const char *kMessagingClientId = "messaging.client.id";

/**
 * The name of the consumer group with which a consumer is associated.
 *
 * <p>Notes:
  <ul> <li>Semantic conventions for individual messaging systems SHOULD document whether {@code
 messaging.consumer.group.name} is applicable and what it means in the context of that system.</li>
 </ul>
 */
static constexpr const char *kMessagingConsumerGroupName = "messaging.consumer.group.name";

/**
 * A boolean that is true if the message destination is anonymous (could be unnamed or have
 * auto-generated name).
 */
static constexpr const char *kMessagingDestinationAnonymous = "messaging.destination.anonymous";

/**
 * The message destination name
 *
 * <p>Notes:
  <ul> <li>Destination name SHOULD uniquely identify a specific queue, topic or other entity within
the broker. If the broker doesn't have such notion, the destination name SHOULD uniquely identify
the broker.</li> </ul>
 */
static constexpr const char *kMessagingDestinationName = "messaging.destination.name";

/**
 * The identifier of the partition messages are sent to or received from, unique within the {@code
 * messaging.destination.name}.
 */
static constexpr const char *kMessagingDestinationPartitionId =
    "messaging.destination.partition.id";

/**
 * The name of the destination subscription from which a message is consumed.
 *
 * <p>Notes:
  <ul> <li>Semantic conventions for individual messaging systems SHOULD document whether {@code
 messaging.destination.subscription.name} is applicable and what it means in the context of that
 system.</li> </ul>
 */
static constexpr const char *kMessagingDestinationSubscriptionName =
    "messaging.destination.subscription.name";

/**
 * Low cardinality representation of the messaging destination name
 *
 * <p>Notes:
  <ul> <li>Destination names could be constructed from templates. An example would be a destination
 name involving a user name or product id. Although the destination name in this case is of high
 cardinality, the underlying template is of low cardinality and can be effectively used for grouping
 and aggregation.</li> </ul>
 */
static constexpr const char *kMessagingDestinationTemplate = "messaging.destination.template";

/**
 * A boolean that is true if the message destination is temporary and might not exist anymore after
 * messages are processed.
 */
static constexpr const char *kMessagingDestinationTemporary = "messaging.destination.temporary";

/**
 * The size of the message body in bytes.
 *
 * <p>Notes:
  <ul> <li>This can refer to both the compressed or uncompressed body size. If both sizes are known,
the uncompressed body size should be used.</li> </ul>
 */
static constexpr const char *kMessagingMessageBodySize = "messaging.message.body.size";

/**
 * The conversation ID identifying the conversation to which the message belongs, represented as a
 * string. Sometimes called &quot;Correlation ID&quot;.
 */
static constexpr const char *kMessagingMessageConversationId = "messaging.message.conversation_id";

/**
 * The size of the message body and metadata in bytes.
 *
 * <p>Notes:
  <ul> <li>This can refer to both the compressed or uncompressed size. If both sizes are known, the
uncompressed size should be used.</li> </ul>
 */
static constexpr const char *kMessagingMessageEnvelopeSize = "messaging.message.envelope.size";

/**
 * A value used by the messaging system as an identifier for the message, represented as a string.
 */
static constexpr const char *kMessagingMessageId = "messaging.message.id";

/**
 * The system-specific name of the messaging operation.
 */
static constexpr const char *kMessagingOperationName = "messaging.operation.name";

/**
 * A string identifying the type of the messaging operation.
 *
 * <p>Notes:
  <ul> <li>If a custom value is used, it MUST be of low cardinality.</li> </ul>
 */
static constexpr const char *kMessagingOperationType = "messaging.operation.type";

/**
 * The messaging system as identified by the client instrumentation.
 *
 * <p>Notes:
  <ul> <li>The actual messaging system may differ from the one known by the client. For example,
 when using Kafka client libraries to communicate with Azure Event Hubs, the {@code
 messaging.system} is set to {@code kafka} based on the instrumentation's best knowledge.</li> </ul>
 */
static constexpr const char *kMessagingSystem = "messaging.system";

/**
 * Message keys in Kafka are used for grouping alike messages to ensure they're processed on the
 same partition. They differ from {@code messaging.message.id} in that they're not unique. If the
 key is {@code null}, the attribute MUST NOT be set.
 *
 * <p>Notes:
  <ul> <li>If the key type is not string, it's string representation has to be supplied for the
 attribute. If the key has no unambiguous, canonical string form, don't include its value.</li>
 </ul>
 */
static constexpr const char *kMessagingKafkaMessageKey = "messaging.kafka.message.key";

/**
 * A boolean that is true if the message is a tombstone.
 */
static constexpr const char *kMessagingKafkaMessageTombstone = "messaging.kafka.message.tombstone";

/**
 * The offset of a record in the corresponding Kafka partition.
 */
static constexpr const char *kMessagingKafkaOffset = "messaging.kafka.offset";

/**
 * RabbitMQ message routing key.
 */
static constexpr const char *kMessagingRabbitmqDestinationRoutingKey =
    "messaging.rabbitmq.destination.routing_key";

/**
 * RabbitMQ message delivery tag
 */
static constexpr const char *kMessagingRabbitmqMessageDeliveryTag =
    "messaging.rabbitmq.message.delivery_tag";

/**
 * Model of message consumption. This only applies to consumer spans.
 */
static constexpr const char *kMessagingRocketmqConsumptionModel =
    "messaging.rocketmq.consumption_model";

/**
 * The delay time level for delay message, which determines the message delay time.
 */
static constexpr const char *kMessagingRocketmqMessageDelayTimeLevel =
    "messaging.rocketmq.message.delay_time_level";

/**
 * The timestamp in milliseconds that the delay message is expected to be delivered to consumer.
 */
static constexpr const char *kMessagingRocketmqMessageDeliveryTimestamp =
    "messaging.rocketmq.message.delivery_timestamp";

/**
 * It is essential for FIFO message. Messages that belong to the same message group are always
 * processed one by one within the same consumer group.
 */
static constexpr const char *kMessagingRocketmqMessageGroup = "messaging.rocketmq.message.group";

/**
 * Key(s) of message, another way to mark message besides message id.
 */
static constexpr const char *kMessagingRocketmqMessageKeys = "messaging.rocketmq.message.keys";

/**
 * The secondary classifier of message besides topic.
 */
static constexpr const char *kMessagingRocketmqMessageTag = "messaging.rocketmq.message.tag";

/**
 * Type of message.
 */
static constexpr const char *kMessagingRocketmqMessageType = "messaging.rocketmq.message.type";

/**
 * Namespace of RocketMQ resources, resources in different namespaces are individual.
 */
static constexpr const char *kMessagingRocketmqNamespace = "messaging.rocketmq.namespace";

/**
 * The ack deadline in seconds set for the modify ack deadline request.
 */
static constexpr const char *kMessagingGcpPubsubMessageAckDeadline =
    "messaging.gcp_pubsub.message.ack_deadline";

/**
 * The ack id for a given message.
 */
static constexpr const char *kMessagingGcpPubsubMessageAckId =
    "messaging.gcp_pubsub.message.ack_id";

/**
 * The delivery attempt for a given message.
 */
static constexpr const char *kMessagingGcpPubsubMessageDeliveryAttempt =
    "messaging.gcp_pubsub.message.delivery_attempt";

/**
 * The ordering key for a given message. If the attribute is not present, the message does not have
 * an ordering key.
 */
static constexpr const char *kMessagingGcpPubsubMessageOrderingKey =
    "messaging.gcp_pubsub.message.ordering_key";

/**
 * Describes the <a
 * href="https://learn.microsoft.com/azure/service-bus-messaging/message-transfers-locks-settlement#peeklock">settlement
 * type</a>.
 */
static constexpr const char *kMessagingServicebusDispositionStatus =
    "messaging.servicebus.disposition_status";

/**
 * Number of deliveries that have been attempted for this message.
 */
static constexpr const char *kMessagingServicebusMessageDeliveryCount =
    "messaging.servicebus.message.delivery_count";

/**
 * The UTC epoch seconds at which the message has been accepted and stored in the entity.
 */
static constexpr const char *kMessagingServicebusMessageEnqueuedTime =
    "messaging.servicebus.message.enqueued_time";

/**
 * The UTC epoch seconds at which the message has been accepted and stored in the entity.
 */
static constexpr const char *kMessagingEventhubsMessageEnqueuedTime =
    "messaging.eventhubs.message.enqueued_time";

/**
 * The ISO 3166-1 alpha-2 2-character country code associated with the mobile carrier network.
 */
static constexpr const char *kNetworkCarrierIcc = "network.carrier.icc";

/**
 * The mobile carrier country code.
 */
static constexpr const char *kNetworkCarrierMcc = "network.carrier.mcc";

/**
 * The mobile carrier network code.
 */
static constexpr const char *kNetworkCarrierMnc = "network.carrier.mnc";

/**
 * The name of the mobile carrier.
 */
static constexpr const char *kNetworkCarrierName = "network.carrier.name";

/**
 * This describes more details regarding the connection.type. It may be the type of cell technology
 * connection, but it could be used for describing details about a wifi connection.
 */
static constexpr const char *kNetworkConnectionSubtype = "network.connection.subtype";

/**
 * The internet connection type.
 */
static constexpr const char *kNetworkConnectionType = "network.connection.type";

/**
 * The network IO operation direction.
 */
static constexpr const char *kNetworkIoDirection = "network.io.direction";

/**
 * Local address of the network connection - IP address or Unix domain socket name.
 */
static constexpr const char *kNetworkLocalAddress = "network.local.address";

/**
 * Local port number of the network connection.
 */
static constexpr const char *kNetworkLocalPort = "network.local.port";

/**
 * Peer address of the network connection - IP address or Unix domain socket name.
 */
static constexpr const char *kNetworkPeerAddress = "network.peer.address";

/**
 * Peer port number of the network connection.
 */
static constexpr const char *kNetworkPeerPort = "network.peer.port";

/**
 * <a href="https://osi-model.com/application-layer/">OSI application layer</a> or non-OSI
 equivalent.
 *
 * <p>Notes:
  <ul> <li>The value SHOULD be normalized to lowercase.</li> </ul>
 */
static constexpr const char *kNetworkProtocolName = "network.protocol.name";

/**
 * The actual version of the protocol used for network communication.
 *
 * <p>Notes:
  <ul> <li>If protocol version is subject to negotiation (for example using <a
 href="https://www.rfc-editor.org/rfc/rfc7301.html">ALPN</a>), this attribute SHOULD be set to the
 negotiated version. If the actual protocol version is not known, this attribute SHOULD NOT be
 set.</li> </ul>
 */
static constexpr const char *kNetworkProtocolVersion = "network.protocol.version";

/**
 * <a href="https://osi-model.com/transport-layer/">OSI transport layer</a> or <a
href="https://wikipedia.org/wiki/Inter-process_communication">inter-process communication
method</a>.
 *
 * <p>Notes:
  <ul> <li>The value SHOULD be normalized to lowercase.</li><li>Consider always setting the
transport when setting a port number, since a port number is ambiguous without knowing the
transport. For example different processes could be listening on TCP port 12345 and UDP port
12345.</li> </ul>
 */
static constexpr const char *kNetworkTransport = "network.transport";

/**
 * <a href="https://osi-model.com/network-layer/">OSI network layer</a> or non-OSI equivalent.
 *
 * <p>Notes:
  <ul> <li>The value SHOULD be normalized to lowercase.</li> </ul>
 */
static constexpr const char *kNetworkType = "network.type";

/**
 * The digest of the OCI image manifest. For container images specifically is the digest by which
the container image is known.
 *
 * <p>Notes:
  <ul> <li>Follows <a href="https://github.com/opencontainers/image-spec/blob/main/manifest.md">OCI
Image Manifest Specification</a>, and specifically the <a
href="https://github.com/opencontainers/image-spec/blob/main/descriptor.md#digests">Digest
property</a>. An example can be found in <a
href="https://docs.docker.com/registry/spec/manifest-v2-2/#example-image-manifest">Example Image
Manifest</a>.</li> </ul>
 */
static constexpr const char *kOciManifestDigest = "oci.manifest.digest";

/**
 * Parent-child Reference type
 *
 * <p>Notes:
  <ul> <li>The causal relationship between a child Span and a parent Span.</li> </ul>
 */
static constexpr const char *kOpentracingRefType = "opentracing.ref_type";

/**
 * Unique identifier for a particular build or compilation of the operating system.
 */
static constexpr const char *kOsBuildId = "os.build_id";

/**
 * Human readable (not intended to be parsed) OS version information, like e.g. reported by {@code
 * ver} or {@code lsb_release -a} commands.
 */
static constexpr const char *kOsDescription = "os.description";

/**
 * Human readable operating system name.
 */
static constexpr const char *kOsName = "os.name";

/**
 * The operating system type.
 */
static constexpr const char *kOsType = "os.type";

/**
 * The version string of the operating system as defined in <a
 * href="/docs/resource/README.md#version-attributes">Version Attributes</a>.
 */
static constexpr const char *kOsVersion = "os.version";

/**
 * Name of the code, either &quot;OK&quot; or &quot;ERROR&quot;. MUST NOT be set if the status code
 * is UNSET.
 */
static constexpr const char *kOtelStatusCode = "otel.status_code";

/**
 * Description of the Status if it has a value, otherwise not set.
 */
static constexpr const char *kOtelStatusDescription = "otel.status_description";

/**
 * The name of the instrumentation scope - ({@code InstrumentationScope.Name} in OTLP).
 */
static constexpr const char *kOtelScopeName = "otel.scope.name";

/**
 * The version of the instrumentation scope - ({@code InstrumentationScope.Version} in OTLP).
 */
static constexpr const char *kOtelScopeVersion = "otel.scope.version";

/**
 * The <a href="/docs/resource/README.md#service">{@code service.name}</a> of the remote service.
 * SHOULD be equal to the actual {@code service.name} resource attribute of the remote service if
 * any.
 */
static constexpr const char *kPeerService = "peer.service";

/**
 * The command used to launch the process (i.e. the command name). On Linux based systems, can be
 * set to the zeroth string in {@code proc/[pid]/cmdline}. On Windows, can be set to the first
 * parameter extracted from {@code GetCommandLineW}.
 */
static constexpr const char *kProcessCommand = "process.command";

/**
 * All the command arguments (including the command/executable itself) as received by the process.
 * On Linux-based systems (and some other Unixoid systems supporting procfs), can be set according
 * to the list of null-delimited strings extracted from {@code proc/[pid]/cmdline}. For libc-based
 * executables, this would be the full argv vector passed to {@code main}.
 */
static constexpr const char *kProcessCommandArgs = "process.command_args";

/**
 * The full command used to launch the process as a single string representing the full command. On
 * Windows, can be set to the result of {@code GetCommandLineW}. Do not set this if you have to
 * assemble it just for monitoring; use {@code process.command_args} instead.
 */
static constexpr const char *kProcessCommandLine = "process.command_line";

/**
 * Specifies whether the context switches for this data point were voluntary or involuntary.
 */
static constexpr const char *kProcessContextSwitchType = "process.context_switch_type";

/**
 * The date and time the process was created, in ISO 8601 format.
 */
static constexpr const char *kProcessCreationTime = "process.creation.time";

/**
 * The name of the process executable. On Linux based systems, can be set to the {@code Name} in
 * {@code proc/[pid]/status}. On Windows, can be set to the base name of {@code
 * GetProcessImageFileNameW}.
 */
static constexpr const char *kProcessExecutableName = "process.executable.name";

/**
 * The full path to the process executable. On Linux based systems, can be set to the target of
 * {@code proc/[pid]/exe}. On Windows, can be set to the result of {@code GetProcessImageFileNameW}.
 */
static constexpr const char *kProcessExecutablePath = "process.executable.path";

/**
 * The exit code of the process.
 */
static constexpr const char *kProcessExitCode = "process.exit.code";

/**
 * The date and time the process exited, in ISO 8601 format.
 */
static constexpr const char *kProcessExitTime = "process.exit.time";

/**
 * The PID of the process's group leader. This is also the process group ID (PGID) of the process.
 */
static constexpr const char *kProcessGroupLeaderPid = "process.group_leader.pid";

/**
 * Whether the process is connected to an interactive shell.
 */
static constexpr const char *kProcessInteractive = "process.interactive";

/**
 * The username of the user that owns the process.
 */
static constexpr const char *kProcessOwner = "process.owner";

/**
 * The type of page fault for this data point. Type {@code major} is for major/hard page faults, and
 * {@code minor} is for minor/soft page faults.
 */
static constexpr const char *kProcessPagingFaultType = "process.paging.fault_type";

/**
 * Parent Process identifier (PPID).
 */
static constexpr const char *kProcessParentPid = "process.parent_pid";

/**
 * Process identifier (PID).
 */
static constexpr const char *kProcessPid = "process.pid";

/**
 * The real user ID (RUID) of the process.
 */
static constexpr const char *kProcessRealUserId = "process.real_user.id";

/**
 * The username of the real user of the process.
 */
static constexpr const char *kProcessRealUserName = "process.real_user.name";

/**
 * An additional description about the runtime of the process, for example a specific vendor
 * customization of the runtime environment.
 */
static constexpr const char *kProcessRuntimeDescription = "process.runtime.description";

/**
 * The name of the runtime of this process.
 */
static constexpr const char *kProcessRuntimeName = "process.runtime.name";

/**
 * The version of the runtime of this process, as returned by the runtime without modification.
 */
static constexpr const char *kProcessRuntimeVersion = "process.runtime.version";

/**
 * The saved user ID (SUID) of the process.
 */
static constexpr const char *kProcessSavedUserId = "process.saved_user.id";

/**
 * The username of the saved user.
 */
static constexpr const char *kProcessSavedUserName = "process.saved_user.name";

/**
 * The PID of the process's session leader. This is also the session ID (SID) of the process.
 */
static constexpr const char *kProcessSessionLeaderPid = "process.session_leader.pid";

/**
 * The effective user ID (EUID) of the process.
 */
static constexpr const char *kProcessUserId = "process.user.id";

/**
 * The username of the effective user of the process.
 */
static constexpr const char *kProcessUserName = "process.user.name";

/**
 * Virtual process identifier.
 *
 * <p>Notes:
  <ul> <li>The process ID within a PID namespace. This is not necessarily unique across all
 processes on the host but it is unique within the process namespace that the process exists
 within.</li> </ul>
 */
static constexpr const char *kProcessVpid = "process.vpid";

/**
 * The <a href="https://connect.build/docs/protocol/#error-codes">error codes</a> of the Connect
 * request. Error codes are always string values.
 */
static constexpr const char *kRpcConnectRpcErrorCode = "rpc.connect_rpc.error_code";

/**
 * The <a href="https://github.com/grpc/grpc/blob/v1.33.2/doc/statuscodes.md">numeric status
 * code</a> of the gRPC request.
 */
static constexpr const char *kRpcGrpcStatusCode = "rpc.grpc.status_code";

/**
 * {@code error.code} property of response if it is an error response.
 */
static constexpr const char *kRpcJsonrpcErrorCode = "rpc.jsonrpc.error_code";

/**
 * {@code error.message} property of response if it is an error response.
 */
static constexpr const char *kRpcJsonrpcErrorMessage = "rpc.jsonrpc.error_message";

/**
 * {@code id} property of request or response. Since protocol allows id to be int, string, {@code
 * null} or missing (for notifications), value is expected to be cast to string for simplicity. Use
 * empty string in case of {@code null} value. Omit entirely if this is a notification.
 */
static constexpr const char *kRpcJsonrpcRequestId = "rpc.jsonrpc.request_id";

/**
 * Protocol version as in {@code jsonrpc} property of request/response. Since JSON-RPC 1.0 doesn't
 * specify this, the value can be omitted.
 */
static constexpr const char *kRpcJsonrpcVersion = "rpc.jsonrpc.version";

/**
 * Compressed size of the message in bytes.
 */
static constexpr const char *kRpcMessageCompressedSize = "rpc.message.compressed_size";

/**
 * MUST be calculated as two different counters starting from {@code 1} one for sent messages and
 one for received message.
 *
 * <p>Notes:
  <ul> <li>This way we guarantee that the values will be consistent between different
 implementations.</li> </ul>
 */
static constexpr const char *kRpcMessageId = "rpc.message.id";

/**
 * Whether this is a received or sent message.
 */
static constexpr const char *kRpcMessageType = "rpc.message.type";

/**
 * Uncompressed size of the message in bytes.
 */
static constexpr const char *kRpcMessageUncompressedSize = "rpc.message.uncompressed_size";

/**
 * The name of the (logical) method being called, must be equal to the $method part in the span
 name.
 *
 * <p>Notes:
  <ul> <li>This is the logical name of the method from the RPC interface perspective, which can be
 different from the name of any implementing method/function. The {@code code.function} attribute
 may be used to store the latter (e.g., method actually executing the call on the server side, RPC
 client stub method on the client side).</li> </ul>
 */
static constexpr const char *kRpcMethod = "rpc.method";

/**
 * The full (logical) name of the service being called, including its package name, if applicable.
 *
 * <p>Notes:
  <ul> <li>This is the logical name of the service from the RPC interface perspective, which can be
 different from the name of any implementing class. The {@code code.namespace} attribute may be used
 to store the latter (despite the attribute name, it may include a class name; e.g., class with
 method actually executing the call on the server side, RPC client stub class on the client
 side).</li> </ul>
 */
static constexpr const char *kRpcService = "rpc.service";

/**
 * A string identifying the remoting system. See below for a list of well-known identifiers.
 */
static constexpr const char *kRpcSystem = "rpc.system";

/**
 * Server domain name if available without reverse DNS lookup; otherwise, IP address or Unix domain
 socket name.
 *
 * <p>Notes:
  <ul> <li>When observed from the client side, and when communicating through an intermediary,
 {@code server.address} SHOULD represent the server address behind any intermediaries, for example
 proxies, if it's available.</li> </ul>
 */
static constexpr const char *kServerAddress = "server.address";

/**
 * Server port number.
 *
 * <p>Notes:
  <ul> <li>When observed from the client side, and when communicating through an intermediary,
 {@code server.port} SHOULD represent the server port behind any intermediaries, for example
 proxies, if it's available.</li> </ul>
 */
static constexpr const char *kServerPort = "server.port";

/**
 * The string ID of the service instance.
 *
 * <p>Notes:
  <ul> <li>MUST be unique for each instance of the same {@code service.namespace,service.name} pair
(in other words
{@code service.namespace,service.name,service.instance.id} triplet MUST be globally unique). The ID
helps to distinguish instances of the same service that exist at the same time (e.g. instances of a
horizontally scaled service).</li><li>Implementations, such as SDKs, are recommended to generate a
random Version 1 or Version 4 <a href="https://www.ietf.org/rfc/rfc4122.txt">RFC 4122</a> UUID, but
are free to use an inherent unique ID as the source of this value if stability is desirable. In that
case, the ID SHOULD be used as source of a UUID Version 5 and SHOULD use the following UUID as the
namespace: {@code 4d63009a-8d0f-11ee-aad7-4c796ed8e320}.</li><li>UUIDs are typically recommended, as
only an opaque value for the purposes of identifying a service instance is needed. Similar to what
can be seen in the man page for the <a
href="https://www.freedesktop.org/software/systemd/man/machine-id.html">{@code /etc/machine-id}</a>
file, the underlying data, such as pod name and namespace should be treated as confidential, being
the user's choice to expose it or not via another resource attribute.</li><li>For applications
running behind an application server (like unicorn), we do not recommend using one identifier for
all processes participating in the application. Instead, it's recommended each division (e.g. a
worker thread in unicorn) to have its own instance.id.</li><li>It's not recommended for a Collector
to set {@code service.instance.id} if it can't unambiguously determine the service instance that is
generating that telemetry. For instance, creating an UUID based on {@code pod.name} will likely be
wrong, as the Collector might not know from which container within that pod the telemetry
originated. However, Collectors can set the {@code service.instance.id} if they can unambiguously
determine the service instance for that telemetry. This is typically the case for scraping
receivers, as they know the target address and port.</li> </ul>
 */
static constexpr const char *kServiceInstanceId = "service.instance.id";

/**
 * Logical name of the service.
 *
 * <p>Notes:
  <ul> <li>MUST be the same for all instances of horizontally scaled services. If the value was not
 specified, SDKs MUST fallback to {@code unknown_service:} concatenated with <a
 href="process.md">{@code process.executable.name}</a>, e.g. {@code unknown_service:bash}. If {@code
 process.executable.name} is not available, the value MUST be set to {@code unknown_service}.</li>
 </ul>
 */
static constexpr const char *kServiceName = "service.name";

/**
 * A namespace for {@code service.name}.
 *
 * <p>Notes:
  <ul> <li>A string value having a meaning that helps to distinguish a group of services, for
 example the team name that owns a group of services. {@code service.name} is expected to be unique
 within the same namespace. If {@code service.namespace} is not specified in the Resource then
 {@code service.name} is expected to be unique for all services that have no explicit namespace
 defined (so the empty/unspecified namespace is simply one more valid namespace). Zero-length
 namespace string is assumed equal to unspecified namespace.</li> </ul>
 */
static constexpr const char *kServiceNamespace = "service.namespace";

/**
 * The version string of the service API or implementation. The format is not defined by these
 * conventions.
 */
static constexpr const char *kServiceVersion = "service.version";

/**
 * A unique id to identify a session.
 */
static constexpr const char *kSessionId = "session.id";

/**
 * The previous {@code session.id} for this user, when known.
 */
static constexpr const char *kSessionPreviousId = "session.previous_id";

/**
 * SignalR HTTP connection closure status.
 */
static constexpr const char *kSignalrConnectionStatus = "signalr.connection.status";

/**
 * <a
 * href="https://github.com/dotnet/aspnetcore/blob/main/src/SignalR/docs/specs/TransportProtocols.md">SignalR
 * transport type</a>
 */
static constexpr const char *kSignalrTransport = "signalr.transport";

/**
 * Source address - domain name if available without reverse DNS lookup; otherwise, IP address or
 Unix domain socket name.
 *
 * <p>Notes:
  <ul> <li>When observed from the destination side, and when communicating through an intermediary,
 {@code source.address} SHOULD represent the source address behind any intermediaries, for example
 proxies, if it's available.</li> </ul>
 */
static constexpr const char *kSourceAddress = "source.address";

/**
 * Source port number
 */
static constexpr const char *kSourcePort = "source.port";

/**
 * The device identifier
 */
static constexpr const char *kSystemDevice = "system.device";

/**
 * The logical CPU number [0..n-1]
 */
static constexpr const char *kSystemCpuLogicalNumber = "system.cpu.logical_number";

/**
 * The memory state
 */
static constexpr const char *kSystemMemoryState = "system.memory.state";

/**
 * The paging access direction
 */
static constexpr const char *kSystemPagingDirection = "system.paging.direction";

/**
 * The memory paging state
 */
static constexpr const char *kSystemPagingState = "system.paging.state";

/**
 * The memory paging type
 */
static constexpr const char *kSystemPagingType = "system.paging.type";

/**
 * The filesystem mode
 */
static constexpr const char *kSystemFilesystemMode = "system.filesystem.mode";

/**
 * The filesystem mount path
 */
static constexpr const char *kSystemFilesystemMountpoint = "system.filesystem.mountpoint";

/**
 * The filesystem state
 */
static constexpr const char *kSystemFilesystemState = "system.filesystem.state";

/**
 * The filesystem type
 */
static constexpr const char *kSystemFilesystemType = "system.filesystem.type";

/**
 * A stateless protocol MUST NOT set this attribute
 */
static constexpr const char *kSystemNetworkState = "system.network.state";

/**
 * The process state, e.g., <a
 * href="https://man7.org/linux/man-pages/man1/ps.1.html#PROCESS_STATE_CODES">Linux Process State
 * Codes</a>
 */
static constexpr const char *kSystemProcessStatus = "system.process.status";

/**
 * The language of the telemetry SDK.
 */
static constexpr const char *kTelemetrySdkLanguage = "telemetry.sdk.language";

/**
 * The name of the telemetry SDK as defined above.
 *
 * <p>Notes:
  <ul> <li>The OpenTelemetry SDK MUST set the {@code telemetry.sdk.name} attribute to {@code
opentelemetry}. If another SDK, like a fork or a vendor-provided implementation, is used, this SDK
MUST set the
{@code telemetry.sdk.name} attribute to the fully-qualified class or module name of this SDK's main
entry point or another suitable identifier depending on the language. The identifier {@code
opentelemetry} is reserved and MUST NOT be used in this case. All custom identifiers SHOULD be
stable across different versions of an implementation.</li> </ul>
 */
static constexpr const char *kTelemetrySdkName = "telemetry.sdk.name";

/**
 * The version string of the telemetry SDK.
 */
static constexpr const char *kTelemetrySdkVersion = "telemetry.sdk.version";

/**
 * The name of the auto instrumentation agent or distribution, if used.
 *
 * <p>Notes:
  <ul> <li>Official auto instrumentation agents and distributions SHOULD set the {@code
telemetry.distro.name} attribute to a string starting with {@code opentelemetry-}, e.g. {@code
opentelemetry-java-instrumentation}.</li> </ul>
 */
static constexpr const char *kTelemetryDistroName = "telemetry.distro.name";

/**
 * The version string of the auto instrumentation agent or distribution, if used.
 */
static constexpr const char *kTelemetryDistroVersion = "telemetry.distro.version";

/**
 * The fully qualified human readable name of the <a
 * href="https://en.wikipedia.org/wiki/Test_case">test case</a>.
 */
static constexpr const char *kTestCaseName = "test.case.name";

/**
 * The status of the actual test case result from test execution.
 */
static constexpr const char *kTestCaseResultStatus = "test.case.result.status";

/**
 * The human readable name of a <a href="https://en.wikipedia.org/wiki/Test_suite">test suite</a>.
 */
static constexpr const char *kTestSuiteName = "test.suite.name";

/**
 * The status of the test suite run.
 */
static constexpr const char *kTestSuiteRunStatus = "test.suite.run.status";

/**
 * Current &quot;managed&quot; thread ID (as opposed to OS thread ID).
 */
static constexpr const char *kThreadId = "thread.id";

/**
 * Current thread name.
 */
static constexpr const char *kThreadName = "thread.name";

/**
 * String indicating the <a
 href="https://datatracker.ietf.org/doc/html/rfc5246#appendix-A.5">cipher</a> used during the
 current connection.
 *
 * <p>Notes:
  <ul> <li>The values allowed for {@code tls.cipher} MUST be one of the {@code Descriptions} of the
 <a
 href="https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#table-tls-parameters-4">registered
 TLS Cipher Suits</a>.</li> </ul>
 */
static constexpr const char *kTlsCipher = "tls.cipher";

/**
 * PEM-encoded stand-alone certificate offered by the client. This is usually mutually-exclusive of
 * {@code client.certificate_chain} since this value also exists in that list.
 */
static constexpr const char *kTlsClientCertificate = "tls.client.certificate";

/**
 * Array of PEM-encoded certificates that make up the certificate chain offered by the client. This
 * is usually mutually-exclusive of {@code client.certificate} since that value should be the first
 * certificate in the chain.
 */
static constexpr const char *kTlsClientCertificateChain = "tls.client.certificate_chain";

/**
 * Certificate fingerprint using the MD5 digest of DER-encoded version of certificate offered by the
 * client. For consistency with other hash values, this value should be formatted as an uppercase
 * hash.
 */
static constexpr const char *kTlsClientHashMd5 = "tls.client.hash.md5";

/**
 * Certificate fingerprint using the SHA1 digest of DER-encoded version of certificate offered by
 * the client. For consistency with other hash values, this value should be formatted as an
 * uppercase hash.
 */
static constexpr const char *kTlsClientHashSha1 = "tls.client.hash.sha1";

/**
 * Certificate fingerprint using the SHA256 digest of DER-encoded version of certificate offered by
 * the client. For consistency with other hash values, this value should be formatted as an
 * uppercase hash.
 */
static constexpr const char *kTlsClientHashSha256 = "tls.client.hash.sha256";

/**
 * Distinguished name of <a
 * href="https://datatracker.ietf.org/doc/html/rfc5280#section-4.1.2.6">subject</a> of the issuer of
 * the x.509 certificate presented by the client.
 */
static constexpr const char *kTlsClientIssuer = "tls.client.issuer";

/**
 * A hash that identifies clients based on how they perform an SSL/TLS handshake.
 */
static constexpr const char *kTlsClientJa3 = "tls.client.ja3";

/**
 * Date/Time indicating when client certificate is no longer considered valid.
 */
static constexpr const char *kTlsClientNotAfter = "tls.client.not_after";

/**
 * Date/Time indicating when client certificate is first considered valid.
 */
static constexpr const char *kTlsClientNotBefore = "tls.client.not_before";

/**
 * Distinguished name of subject of the x.509 certificate presented by the client.
 */
static constexpr const char *kTlsClientSubject = "tls.client.subject";

/**
 * Array of ciphers offered by the client during the client hello.
 */
static constexpr const char *kTlsClientSupportedCiphers = "tls.client.supported_ciphers";

/**
 * String indicating the curve used for the given cipher, when applicable
 */
static constexpr const char *kTlsCurve = "tls.curve";

/**
 * Boolean flag indicating if the TLS negotiation was successful and transitioned to an encrypted
 * tunnel.
 */
static constexpr const char *kTlsEstablished = "tls.established";

/**
 * String indicating the protocol being tunneled. Per the values in the <a
 * href="https://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xhtml#alpn-protocol-ids">IANA
 * registry</a>, this string should be lower case.
 */
static constexpr const char *kTlsNextProtocol = "tls.next_protocol";

/**
 * Normalized lowercase protocol name parsed from original string of the negotiated <a
 * href="https://www.openssl.org/docs/man1.1.1/man3/SSL_get_version.html#RETURN-VALUES">SSL/TLS
 * protocol version</a>
 */
static constexpr const char *kTlsProtocolName = "tls.protocol.name";

/**
 * Numeric part of the version parsed from the original string of the negotiated <a
 * href="https://www.openssl.org/docs/man1.1.1/man3/SSL_get_version.html#RETURN-VALUES">SSL/TLS
 * protocol version</a>
 */
static constexpr const char *kTlsProtocolVersion = "tls.protocol.version";

/**
 * Boolean flag indicating if this TLS connection was resumed from an existing TLS negotiation.
 */
static constexpr const char *kTlsResumed = "tls.resumed";

/**
 * PEM-encoded stand-alone certificate offered by the server. This is usually mutually-exclusive of
 * {@code server.certificate_chain} since this value also exists in that list.
 */
static constexpr const char *kTlsServerCertificate = "tls.server.certificate";

/**
 * Array of PEM-encoded certificates that make up the certificate chain offered by the server. This
 * is usually mutually-exclusive of {@code server.certificate} since that value should be the first
 * certificate in the chain.
 */
static constexpr const char *kTlsServerCertificateChain = "tls.server.certificate_chain";

/**
 * Certificate fingerprint using the MD5 digest of DER-encoded version of certificate offered by the
 * server. For consistency with other hash values, this value should be formatted as an uppercase
 * hash.
 */
static constexpr const char *kTlsServerHashMd5 = "tls.server.hash.md5";

/**
 * Certificate fingerprint using the SHA1 digest of DER-encoded version of certificate offered by
 * the server. For consistency with other hash values, this value should be formatted as an
 * uppercase hash.
 */
static constexpr const char *kTlsServerHashSha1 = "tls.server.hash.sha1";

/**
 * Certificate fingerprint using the SHA256 digest of DER-encoded version of certificate offered by
 * the server. For consistency with other hash values, this value should be formatted as an
 * uppercase hash.
 */
static constexpr const char *kTlsServerHashSha256 = "tls.server.hash.sha256";

/**
 * Distinguished name of <a
 * href="https://datatracker.ietf.org/doc/html/rfc5280#section-4.1.2.6">subject</a> of the issuer of
 * the x.509 certificate presented by the client.
 */
static constexpr const char *kTlsServerIssuer = "tls.server.issuer";

/**
 * A hash that identifies servers based on how they perform an SSL/TLS handshake.
 */
static constexpr const char *kTlsServerJa3s = "tls.server.ja3s";

/**
 * Date/Time indicating when server certificate is no longer considered valid.
 */
static constexpr const char *kTlsServerNotAfter = "tls.server.not_after";

/**
 * Date/Time indicating when server certificate is first considered valid.
 */
static constexpr const char *kTlsServerNotBefore = "tls.server.not_before";

/**
 * Distinguished name of subject of the x.509 certificate presented by the server.
 */
static constexpr const char *kTlsServerSubject = "tls.server.subject";

/**
 * Domain extracted from the {@code url.full}, such as &quot;opentelemetry.io&quot;.
 *
 * <p>Notes:
  <ul> <li>In some cases a URL may refer to an IP and/or port directly, without a domain name. In
 this case, the IP address would go to the domain field. If the URL contains a <a
 href="https://www.rfc-editor.org/rfc/rfc2732#section-2">literal IPv6 address</a> enclosed by {@code
 [} and {@code ]}, the {@code [} and {@code ]} characters should also be captured in the domain
 field.</li> </ul>
 */
static constexpr const char *kUrlDomain = "url.domain";

/**
 * The file extension extracted from the {@code url.full}, excluding the leading dot.
 *
 * <p>Notes:
  <ul> <li>The file extension is only set if it exists, as not every url has a file extension. When
 the file name has multiple extensions {@code example.tar.gz}, only the last one should be captured
 {@code gz}, not {@code tar.gz}.</li> </ul>
 */
static constexpr const char *kUrlExtension = "url.extension";

/**
 * The <a href="https://www.rfc-editor.org/rfc/rfc3986#section-3.5">URI fragment</a> component
 */
static constexpr const char *kUrlFragment = "url.fragment";

/**
 * Absolute URL describing a network resource according to <a
href="https://www.rfc-editor.org/rfc/rfc3986">RFC3986</a>
 *
 * <p>Notes:
  <ul> <li>For network calls, URL usually has {@code scheme://host[:port][path][?query][#fragment]}
format, where the fragment is not transmitted over HTTP, but if it is known, it SHOULD be included
nevertheless.
{@code url.full} MUST NOT contain credentials passed via URL in form of {@code
https://username:password@www.example.com/}. In such case username and password SHOULD be redacted
and attribute's value SHOULD be {@code https://REDACTED:REDACTED@www.example.com/}.
{@code url.full} SHOULD capture the absolute URL when it is available (or can be reconstructed).
Sensitive content provided in {@code url.full} SHOULD be scrubbed when instrumentations can identify
it.</li> </ul>
 */
static constexpr const char *kUrlFull = "url.full";

/**
 * Unmodified original URL as seen in the event source.
 *
 * <p>Notes:
  <ul> <li>In network monitoring, the observed URL may be a full URL, whereas in access logs, the
URL is often just represented as a path. This field is meant to represent the URL as it was
observed, complete or not.
{@code url.original} might contain credentials passed via URL in form of {@code
https://username:password@www.example.com/}. In such case password and username SHOULD NOT be
redacted and attribute's value SHOULD remain the same.</li> </ul>
 */
static constexpr const char *kUrlOriginal = "url.original";

/**
 * The <a href="https://www.rfc-editor.org/rfc/rfc3986#section-3.3">URI path</a> component
 *
 * <p>Notes:
  <ul> <li>Sensitive content provided in {@code url.path} SHOULD be scrubbed when instrumentations
 can identify it.</li> </ul>
 */
static constexpr const char *kUrlPath = "url.path";

/**
 * Port extracted from the {@code url.full}
 */
static constexpr const char *kUrlPort = "url.port";

/**
 * The <a href="https://www.rfc-editor.org/rfc/rfc3986#section-3.4">URI query</a> component
 *
 * <p>Notes:
  <ul> <li>Sensitive content provided in {@code url.query} SHOULD be scrubbed when instrumentations
 can identify it.</li> </ul>
 */
static constexpr const char *kUrlQuery = "url.query";

/**
 * The highest registered url domain, stripped of the subdomain.
 *
 * <p>Notes:
  <ul> <li>This value can be determined precisely with the <a href="http://publicsuffix.org">public
 suffix list</a>. For example, the registered domain for {@code foo.example.com} is {@code
 example.com}. Trying to approximate this by simply taking the last two labels will not work well
 for TLDs such as {@code co.uk}.</li> </ul>
 */
static constexpr const char *kUrlRegisteredDomain = "url.registered_domain";

/**
 * The <a href="https://www.rfc-editor.org/rfc/rfc3986#section-3.1">URI scheme</a> component
 * identifying the used protocol.
 */
static constexpr const char *kUrlScheme = "url.scheme";

/**
 * The subdomain portion of a fully qualified domain name includes all of the names except the host
 name under the registered_domain. In a partially qualified domain, or if the qualification level of
 the full name cannot be determined, subdomain contains all of the names below the registered
 domain.
 *
 * <p>Notes:
  <ul> <li>The subdomain portion of {@code www.east.mydomain.co.uk} is {@code east}. If the domain
 has multiple levels of subdomain, such as {@code sub2.sub1.example.com}, the subdomain field should
 contain {@code sub2.sub1}, with no trailing period.</li> </ul>
 */
static constexpr const char *kUrlSubdomain = "url.subdomain";

/**
 * The low-cardinality template of an <a
 * href="https://www.rfc-editor.org/rfc/rfc3986#section-4.2">absolute path reference</a>.
 */
static constexpr const char *kUrlTemplate = "url.template";

/**
 * The effective top level domain (eTLD), also known as the domain suffix, is the last part of the
 domain name. For example, the top level domain for example.com is {@code com}.
 *
 * <p>Notes:
  <ul> <li>This value can be determined precisely with the <a href="http://publicsuffix.org">public
 suffix list</a>.</li> </ul>
 */
static constexpr const char *kUrlTopLevelDomain = "url.top_level_domain";

/**
 * Name of the user-agent extracted from original. Usually refers to the browser's name.
 *
 * <p>Notes:
  <ul> <li><a href="https://www.whatsmyua.info">Example</a> of extracting browser's name from
 original string. In the case of using a user-agent for non-browser products, such as microservices
 with multiple names/versions inside the {@code user_agent.original}, the most significant name
 SHOULD be selected. In such a scenario it should align with {@code user_agent.version}</li> </ul>
 */
static constexpr const char *kUserAgentName = "user_agent.name";

/**
 * Value of the <a href="https://www.rfc-editor.org/rfc/rfc9110.html#field.user-agent">HTTP
 * User-Agent</a> header sent by the client.
 */
static constexpr const char *kUserAgentOriginal = "user_agent.original";

/**
 * Version of the user-agent extracted from original. Usually refers to the browser's version
 *
 * <p>Notes:
  <ul> <li><a href="https://www.whatsmyua.info">Example</a> of extracting browser's version from
 original string. In the case of using a user-agent for non-browser products, such as microservices
 with multiple names/versions inside the {@code user_agent.original}, the most significant version
 SHOULD be selected. In such a scenario it should align with {@code user_agent.name}</li> </ul>
 */
static constexpr const char *kUserAgentVersion = "user_agent.version";

/**
 * User email address.
 */
static constexpr const char *kUserEmail = "user.email";

/**
 * User's full name
 */
static constexpr const char *kUserFullName = "user.full_name";

/**
 * Unique user hash to correlate information for a user in anonymized form.
 *
 * <p>Notes:
  <ul> <li>Useful if {@code user.id} or {@code user.name} contain confidential information and
 cannot be used.</li> </ul>
 */
static constexpr const char *kUserHash = "user.hash";

/**
 * Unique identifier of the user.
 */
static constexpr const char *kUserId = "user.id";

/**
 * Short name or login/username of the user.
 */
static constexpr const char *kUserName = "user.name";

/**
 * Array of user roles at the time of the event.
 */
static constexpr const char *kUserRoles = "user.roles";

/**
 * The type of garbage collection.
 */
static constexpr const char *kV8jsGcType = "v8js.gc.type";

/**
 * The name of the space type of heap memory.
 *
 * <p>Notes:
  <ul> <li>Value can be retrieved from value {@code space_name} of <a
 href="https://nodejs.org/api/v8.html#v8getheapspacestatistics">{@code
 v8.getHeapSpaceStatistics()}</a></li> </ul>
 */
static constexpr const char *kV8jsHeapSpaceName = "v8js.heap.space.name";

/**
 * The ID of the change (pull request/merge request) if applicable. This is usually a unique (within
 * repository) identifier generated by the VCS system.
 */
static constexpr const char *kVcsRepositoryChangeId = "vcs.repository.change.id";

/**
 * The human readable title of the change (pull request/merge request). This title is often a brief
 * summary of the change and may get merged in to a ref as the commit summary.
 */
static constexpr const char *kVcsRepositoryChangeTitle = "vcs.repository.change.title";

/**
 * The name of the <a href="https://git-scm.com/docs/gitglossary#def_ref">reference</a> such as
 * <strong>branch</strong> or <strong>tag</strong> in the repository.
 */
static constexpr const char *kVcsRepositoryRefName = "vcs.repository.ref.name";

/**
 * The revision, literally <a href="https://www.merriam-webster.com/dictionary/revision">revised
version</a>, The revision most often refers to a commit object in Git, or a revision number in SVN.
 *
 * <p>Notes:
  <ul> <li>The revision can be a full <a
href="https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.186-5.pdf">hash value (see glossary)</a>, of
the recorded change to a ref within a repository pointing to a commit <a
href="https://git-scm.com/docs/git-commit">commit</a> object. It does not necessarily have to be a
hash; it can simply define a <a
href="https://svnbook.red-bean.com/en/1.7/svn.tour.revs.specifiers.html">revision number</a> which
is an integer that is monotonically increasing. In cases where it is identical to the {@code
ref.name}, it SHOULD still be included. It is up to the implementer to decide which value to set as
the revision based on the VCS system and situational context.</li> </ul>
 */
static constexpr const char *kVcsRepositoryRefRevision = "vcs.repository.ref.revision";

/**
 * The type of the <a href="https://git-scm.com/docs/gitglossary#def_ref">reference</a> in the
 * repository.
 */
static constexpr const char *kVcsRepositoryRefType = "vcs.repository.ref.type";

/**
 * The <a href="https://en.wikipedia.org/wiki/URL">URL</a> of the repository providing the complete
 * address in order to locate and identify the repository.
 */
static constexpr const char *kVcsRepositoryUrlFull = "vcs.repository.url.full";

/**
 * Additional description of the web engine (e.g. detailed version and edition information).
 */
static constexpr const char *kWebengineDescription = "webengine.description";

/**
 * The name of the web engine.
 */
static constexpr const char *kWebengineName = "webengine.name";

/**
 * The version of the web engine.
 */
static constexpr const char *kWebengineVersion = "webengine.version";

// Enum definitions
namespace AspnetcoreRateLimitingResultValues
{
/** Lease was acquired. */
static constexpr const char *kAcquired = "acquired";
/** Lease request was rejected by the endpoint limiter. */
static constexpr const char *kEndpointLimiter = "endpoint_limiter";
/** Lease request was rejected by the global limiter. */
static constexpr const char *kGlobalLimiter = "global_limiter";
/** Lease request was canceled. */
static constexpr const char *kRequestCanceled = "request_canceled";
}  // namespace AspnetcoreRateLimitingResultValues

namespace AspnetcoreDiagnosticsExceptionResultValues
{
/** Exception was handled by the exception handling middleware. */
static constexpr const char *kHandled = "handled";
/** Exception was not handled by the exception handling middleware. */
static constexpr const char *kUnhandled = "unhandled";
/** Exception handling was skipped because the response had started. */
static constexpr const char *kSkipped = "skipped";
/** Exception handling didn&#39;t run because the request was aborted. */
static constexpr const char *kAborted = "aborted";
}  // namespace AspnetcoreDiagnosticsExceptionResultValues

namespace AspnetcoreRoutingMatchStatusValues
{
/** Match succeeded. */
static constexpr const char *kSuccess = "success";
/** Match failed. */
static constexpr const char *kFailure = "failure";
}  // namespace AspnetcoreRoutingMatchStatusValues

namespace AwsEcsLaunchtypeValues
{
/** ec2. */
static constexpr const char *kEc2 = "ec2";
/** fargate. */
static constexpr const char *kFargate = "fargate";
}  // namespace AwsEcsLaunchtypeValues

namespace CicdPipelineTaskTypeValues
{
/** build. */
static constexpr const char *kBuild = "build";
/** test. */
static constexpr const char *kTest = "test";
/** deploy. */
static constexpr const char *kDeploy = "deploy";
}  // namespace CicdPipelineTaskTypeValues

namespace CloudPlatformValues
{
/** Alibaba Cloud Elastic Compute Service. */
static constexpr const char *kAlibabaCloudEcs = "alibaba_cloud_ecs";
/** Alibaba Cloud Function Compute. */
static constexpr const char *kAlibabaCloudFc = "alibaba_cloud_fc";
/** Red Hat OpenShift on Alibaba Cloud. */
static constexpr const char *kAlibabaCloudOpenshift = "alibaba_cloud_openshift";
/** AWS Elastic Compute Cloud. */
static constexpr const char *kAwsEc2 = "aws_ec2";
/** AWS Elastic Container Service. */
static constexpr const char *kAwsEcs = "aws_ecs";
/** AWS Elastic Kubernetes Service. */
static constexpr const char *kAwsEks = "aws_eks";
/** AWS Lambda. */
static constexpr const char *kAwsLambda = "aws_lambda";
/** AWS Elastic Beanstalk. */
static constexpr const char *kAwsElasticBeanstalk = "aws_elastic_beanstalk";
/** AWS App Runner. */
static constexpr const char *kAwsAppRunner = "aws_app_runner";
/** Red Hat OpenShift on AWS (ROSA). */
static constexpr const char *kAwsOpenshift = "aws_openshift";
/** Azure Virtual Machines. */
static constexpr const char *kAzureVm = "azure_vm";
/** Azure Container Apps. */
static constexpr const char *kAzureContainerApps = "azure_container_apps";
/** Azure Container Instances. */
static constexpr const char *kAzureContainerInstances = "azure_container_instances";
/** Azure Kubernetes Service. */
static constexpr const char *kAzureAks = "azure_aks";
/** Azure Functions. */
static constexpr const char *kAzureFunctions = "azure_functions";
/** Azure App Service. */
static constexpr const char *kAzureAppService = "azure_app_service";
/** Azure Red Hat OpenShift. */
static constexpr const char *kAzureOpenshift = "azure_openshift";
/** Google Bare Metal Solution (BMS). */
static constexpr const char *kGcpBareMetalSolution = "gcp_bare_metal_solution";
/** Google Cloud Compute Engine (GCE). */
static constexpr const char *kGcpComputeEngine = "gcp_compute_engine";
/** Google Cloud Run. */
static constexpr const char *kGcpCloudRun = "gcp_cloud_run";
/** Google Cloud Kubernetes Engine (GKE). */
static constexpr const char *kGcpKubernetesEngine = "gcp_kubernetes_engine";
/** Google Cloud Functions (GCF). */
static constexpr const char *kGcpCloudFunctions = "gcp_cloud_functions";
/** Google Cloud App Engine (GAE). */
static constexpr const char *kGcpAppEngine = "gcp_app_engine";
/** Red Hat OpenShift on Google Cloud. */
static constexpr const char *kGcpOpenshift = "gcp_openshift";
/** Red Hat OpenShift on IBM Cloud. */
static constexpr const char *kIbmCloudOpenshift = "ibm_cloud_openshift";
/** Tencent Cloud Cloud Virtual Machine (CVM). */
static constexpr const char *kTencentCloudCvm = "tencent_cloud_cvm";
/** Tencent Cloud Elastic Kubernetes Service (EKS). */
static constexpr const char *kTencentCloudEks = "tencent_cloud_eks";
/** Tencent Cloud Serverless Cloud Function (SCF). */
static constexpr const char *kTencentCloudScf = "tencent_cloud_scf";
}  // namespace CloudPlatformValues

namespace CloudProviderValues
{
/** Alibaba Cloud. */
static constexpr const char *kAlibabaCloud = "alibaba_cloud";
/** Amazon Web Services. */
static constexpr const char *kAws = "aws";
/** Microsoft Azure. */
static constexpr const char *kAzure = "azure";
/** Google Cloud Platform. */
static constexpr const char *kGcp = "gcp";
/** Heroku Platform as a Service. */
static constexpr const char *kHeroku = "heroku";
/** IBM Cloud. */
static constexpr const char *kIbmCloud = "ibm_cloud";
/** Tencent Cloud. */
static constexpr const char *kTencentCloud = "tencent_cloud";
}  // namespace CloudProviderValues

namespace CpuModeValues
{
/** user. */
static constexpr const char *kUser = "user";
/** system. */
static constexpr const char *kSystem = "system";
/** nice. */
static constexpr const char *kNice = "nice";
/** idle. */
static constexpr const char *kIdle = "idle";
/** iowait. */
static constexpr const char *kIowait = "iowait";
/** interrupt. */
static constexpr const char *kInterrupt = "interrupt";
/** steal. */
static constexpr const char *kSteal = "steal";
/** kernel. */
static constexpr const char *kKernel = "kernel";
}  // namespace CpuModeValues

namespace DbClientConnectionStateValues
{
/** idle. */
static constexpr const char *kIdle = "idle";
/** used. */
static constexpr const char *kUsed = "used";
}  // namespace DbClientConnectionStateValues

namespace DbSystemValues
{
/** Some other SQL database. Fallback only. See notes. */
static constexpr const char *kOtherSql = "other_sql";
/** Adabas (Adaptable Database System). */
static constexpr const char *kAdabas = "adabas";
/** Deprecated, use `intersystems_cache` instead. */
static constexpr const char *kCache = "cache";
/** InterSystems Caché. */
static constexpr const char *kIntersystemsCache = "intersystems_cache";
/** Apache Cassandra. */
static constexpr const char *kCassandra = "cassandra";
/** ClickHouse. */
static constexpr const char *kClickhouse = "clickhouse";
/** Deprecated, use `other_sql` instead. */
static constexpr const char *kCloudscape = "cloudscape";
/** CockroachDB. */
static constexpr const char *kCockroachdb = "cockroachdb";
/** Deprecated, no replacement at this time. */
static constexpr const char *kColdfusion = "coldfusion";
/** Microsoft Azure Cosmos DB. */
static constexpr const char *kCosmosdb = "cosmosdb";
/** Couchbase. */
static constexpr const char *kCouchbase = "couchbase";
/** CouchDB. */
static constexpr const char *kCouchdb = "couchdb";
/** IBM Db2. */
static constexpr const char *kDb2 = "db2";
/** Apache Derby. */
static constexpr const char *kDerby = "derby";
/** Amazon DynamoDB. */
static constexpr const char *kDynamodb = "dynamodb";
/** EnterpriseDB. */
static constexpr const char *kEdb = "edb";
/** Elasticsearch. */
static constexpr const char *kElasticsearch = "elasticsearch";
/** FileMaker. */
static constexpr const char *kFilemaker = "filemaker";
/** Firebird. */
static constexpr const char *kFirebird = "firebird";
/** Deprecated, use `other_sql` instead. */
static constexpr const char *kFirstsql = "firstsql";
/** Apache Geode. */
static constexpr const char *kGeode = "geode";
/** H2. */
static constexpr const char *kH2 = "h2";
/** SAP HANA. */
static constexpr const char *kHanadb = "hanadb";
/** Apache HBase. */
static constexpr const char *kHbase = "hbase";
/** Apache Hive. */
static constexpr const char *kHive = "hive";
/** HyperSQL DataBase. */
static constexpr const char *kHsqldb = "hsqldb";
/** InfluxDB. */
static constexpr const char *kInfluxdb = "influxdb";
/** Informix. */
static constexpr const char *kInformix = "informix";
/** Ingres. */
static constexpr const char *kIngres = "ingres";
/** InstantDB. */
static constexpr const char *kInstantdb = "instantdb";
/** InterBase. */
static constexpr const char *kInterbase = "interbase";
/** MariaDB. */
static constexpr const char *kMariadb = "mariadb";
/** SAP MaxDB. */
static constexpr const char *kMaxdb = "maxdb";
/** Memcached. */
static constexpr const char *kMemcached = "memcached";
/** MongoDB. */
static constexpr const char *kMongodb = "mongodb";
/** Microsoft SQL Server. */
static constexpr const char *kMssql = "mssql";
/** Deprecated, Microsoft SQL Server Compact is discontinued. */
static constexpr const char *kMssqlcompact = "mssqlcompact";
/** MySQL. */
static constexpr const char *kMysql = "mysql";
/** Neo4j. */
static constexpr const char *kNeo4j = "neo4j";
/** Netezza. */
static constexpr const char *kNetezza = "netezza";
/** OpenSearch. */
static constexpr const char *kOpensearch = "opensearch";
/** Oracle Database. */
static constexpr const char *kOracle = "oracle";
/** Pervasive PSQL. */
static constexpr const char *kPervasive = "pervasive";
/** PointBase. */
static constexpr const char *kPointbase = "pointbase";
/** PostgreSQL. */
static constexpr const char *kPostgresql = "postgresql";
/** Progress Database. */
static constexpr const char *kProgress = "progress";
/** Redis. */
static constexpr const char *kRedis = "redis";
/** Amazon Redshift. */
static constexpr const char *kRedshift = "redshift";
/** Cloud Spanner. */
static constexpr const char *kSpanner = "spanner";
/** SQLite. */
static constexpr const char *kSqlite = "sqlite";
/** Sybase. */
static constexpr const char *kSybase = "sybase";
/** Teradata. */
static constexpr const char *kTeradata = "teradata";
/** Trino. */
static constexpr const char *kTrino = "trino";
/** Vertica. */
static constexpr const char *kVertica = "vertica";
}  // namespace DbSystemValues

namespace DbCassandraConsistencyLevelValues
{
/** all. */
static constexpr const char *kAll = "all";
/** each_quorum. */
static constexpr const char *kEachQuorum = "each_quorum";
/** quorum. */
static constexpr const char *kQuorum = "quorum";
/** local_quorum. */
static constexpr const char *kLocalQuorum = "local_quorum";
/** one. */
static constexpr const char *kOne = "one";
/** two. */
static constexpr const char *kTwo = "two";
/** three. */
static constexpr const char *kThree = "three";
/** local_one. */
static constexpr const char *kLocalOne = "local_one";
/** any. */
static constexpr const char *kAny = "any";
/** serial. */
static constexpr const char *kSerial = "serial";
/** local_serial. */
static constexpr const char *kLocalSerial = "local_serial";
}  // namespace DbCassandraConsistencyLevelValues

namespace DbCosmosdbConnectionModeValues
{
/** Gateway (HTTP) connections mode. */
static constexpr const char *kGateway = "gateway";
/** Direct connection. */
static constexpr const char *kDirect = "direct";
}  // namespace DbCosmosdbConnectionModeValues

namespace DbCosmosdbOperationTypeValues
{
/** invalid. */
static constexpr const char *kInvalid = "Invalid";
/** create. */
static constexpr const char *kCreate = "Create";
/** patch. */
static constexpr const char *kPatch = "Patch";
/** read. */
static constexpr const char *kRead = "Read";
/** read_feed. */
static constexpr const char *kReadFeed = "ReadFeed";
/** delete. */
static constexpr const char *kDelete = "Delete";
/** replace. */
static constexpr const char *kReplace = "Replace";
/** execute. */
static constexpr const char *kExecute = "Execute";
/** query. */
static constexpr const char *kQuery = "Query";
/** head. */
static constexpr const char *kHead = "Head";
/** head_feed. */
static constexpr const char *kHeadFeed = "HeadFeed";
/** upsert. */
static constexpr const char *kUpsert = "Upsert";
/** batch. */
static constexpr const char *kBatch = "Batch";
/** query_plan. */
static constexpr const char *kQueryPlan = "QueryPlan";
/** execute_javascript. */
static constexpr const char *kExecuteJavascript = "ExecuteJavaScript";
}  // namespace DbCosmosdbOperationTypeValues

namespace DeploymentStatusValues
{
/** failed. */
static constexpr const char *kFailed = "failed";
/** succeeded. */
static constexpr const char *kSucceeded = "succeeded";
}  // namespace DeploymentStatusValues

namespace AndroidStateValues
{
/** Any time before Activity.onResume() or, if the app has no Activity, Context.startService() has
 * been called in the app for the first time. */
static constexpr const char *kCreated = "created";
/** Any time after Activity.onPause() or, if the app has no Activity, Context.stopService() has been
 * called when the app was in the foreground state. */
static constexpr const char *kBackground = "background";
/** Any time after Activity.onResume() or, if the app has no Activity, Context.startService() has
 * been called when the app was in either the created or background states. */
static constexpr const char *kForeground = "foreground";
}  // namespace AndroidStateValues

namespace ContainerCpuStateValues
{
/** When tasks of the cgroup are in user mode (Linux). When all container processes are in user mode
 * (Windows). */
static constexpr const char *kUser = "user";
/** When CPU is used by the system (host OS). */
static constexpr const char *kSystem = "system";
/** When tasks of the cgroup are in kernel mode (Linux). When all container processes are in kernel
 * mode (Windows). */
static constexpr const char *kKernel = "kernel";
}  // namespace ContainerCpuStateValues

namespace DbClientConnectionsStateValues
{
/** idle. */
static constexpr const char *kIdle = "idle";
/** used. */
static constexpr const char *kUsed = "used";
}  // namespace DbClientConnectionsStateValues

namespace StateValues
{
/** idle. */
static constexpr const char *kIdle = "idle";
/** used. */
static constexpr const char *kUsed = "used";
}  // namespace StateValues

namespace HttpFlavorValues
{
/** HTTP/1.0. */
static constexpr const char *kHttp10 = "1.0";
/** HTTP/1.1. */
static constexpr const char *kHttp11 = "1.1";
/** HTTP/2. */
static constexpr const char *kHttp20 = "2.0";
/** HTTP/3. */
static constexpr const char *kHttp30 = "3.0";
/** SPDY protocol. */
static constexpr const char *kSpdy = "SPDY";
/** QUIC protocol. */
static constexpr const char *kQuic = "QUIC";
}  // namespace HttpFlavorValues

namespace IosStateValues
{
/** The app has become `active`. Associated with UIKit notification `applicationDidBecomeActive`. */
static constexpr const char *kActive = "active";
/** The app is now `inactive`. Associated with UIKit notification `applicationWillResignActive`. */
static constexpr const char *kInactive = "inactive";
/** The app is now in the background. This value is associated with UIKit notification
 * `applicationDidEnterBackground`. */
static constexpr const char *kBackground = "background";
/** The app is now in the foreground. This value is associated with UIKit notification
 * `applicationWillEnterForeground`. */
static constexpr const char *kForeground = "foreground";
/** The app is about to terminate. Associated with UIKit notification `applicationWillTerminate`. */
static constexpr const char *kTerminate = "terminate";
}  // namespace IosStateValues

namespace NetSockFamilyValues
{
/** IPv4 address. */
static constexpr const char *kInet = "inet";
/** IPv6 address. */
static constexpr const char *kInet6 = "inet6";
/** Unix domain socket path. */
static constexpr const char *kUnix = "unix";
}  // namespace NetSockFamilyValues

namespace NetTransportValues
{
/** ip_tcp. */
static constexpr const char *kIpTcp = "ip_tcp";
/** ip_udp. */
static constexpr const char *kIpUdp = "ip_udp";
/** Named or anonymous pipe. */
static constexpr const char *kPipe = "pipe";
/** In-process communication. */
static constexpr const char *kInproc = "inproc";
/** Something else (non IP-based). */
static constexpr const char *kOther = "other";
}  // namespace NetTransportValues

namespace ProcessCpuStateValues
{
/** system. */
static constexpr const char *kSystem = "system";
/** user. */
static constexpr const char *kUser = "user";
/** wait. */
static constexpr const char *kWait = "wait";
}  // namespace ProcessCpuStateValues

namespace MessageTypeValues
{
/** sent. */
static constexpr const char *kSent = "SENT";
/** received. */
static constexpr const char *kReceived = "RECEIVED";
}  // namespace MessageTypeValues

namespace SystemCpuStateValues
{
/** user. */
static constexpr const char *kUser = "user";
/** system. */
static constexpr const char *kSystem = "system";
/** nice. */
static constexpr const char *kNice = "nice";
/** idle. */
static constexpr const char *kIdle = "idle";
/** iowait. */
static constexpr const char *kIowait = "iowait";
/** interrupt. */
static constexpr const char *kInterrupt = "interrupt";
/** steal. */
static constexpr const char *kSteal = "steal";
}  // namespace SystemCpuStateValues

namespace SystemProcessesStatusValues
{
/** running. */
static constexpr const char *kRunning = "running";
/** sleeping. */
static constexpr const char *kSleeping = "sleeping";
/** stopped. */
static constexpr const char *kStopped = "stopped";
/** defunct. */
static constexpr const char *kDefunct = "defunct";
}  // namespace SystemProcessesStatusValues

namespace DiskIoDirectionValues
{
/** read. */
static constexpr const char *kRead = "read";
/** write. */
static constexpr const char *kWrite = "write";
}  // namespace DiskIoDirectionValues

namespace ErrorTypeValues
{
/** A fallback error value to be used when the instrumentation doesn&#39;t define a custom value. */
static constexpr const char *kOther = "_OTHER";
}  // namespace ErrorTypeValues

namespace FaasDocumentOperationValues
{
/** When a new object is created. */
static constexpr const char *kInsert = "insert";
/** When an object is modified. */
static constexpr const char *kEdit = "edit";
/** When an object is deleted. */
static constexpr const char *kDelete = "delete";
}  // namespace FaasDocumentOperationValues

namespace FaasInvokedProviderValues
{
/** Alibaba Cloud. */
static constexpr const char *kAlibabaCloud = "alibaba_cloud";
/** Amazon Web Services. */
static constexpr const char *kAws = "aws";
/** Microsoft Azure. */
static constexpr const char *kAzure = "azure";
/** Google Cloud Platform. */
static constexpr const char *kGcp = "gcp";
/** Tencent Cloud. */
static constexpr const char *kTencentCloud = "tencent_cloud";
}  // namespace FaasInvokedProviderValues

namespace FaasTriggerValues
{
/** A response to some data source operation such as a database or filesystem read/write. */
static constexpr const char *kDatasource = "datasource";
/** To provide an answer to an inbound HTTP request. */
static constexpr const char *kHttp = "http";
/** A function is set to be executed when messages are sent to a messaging system. */
static constexpr const char *kPubsub = "pubsub";
/** A function is scheduled to be executed regularly. */
static constexpr const char *kTimer = "timer";
/** If none of the others apply. */
static constexpr const char *kOther = "other";
}  // namespace FaasTriggerValues

namespace GenAiOperationNameValues
{
/** Chat completion operation such as [OpenAI Chat
 * API](https://platform.openai.com/docs/api-reference/chat). */
static constexpr const char *kChat = "chat";
/** Text completions operation such as [OpenAI Completions API
 * (Legacy)](https://platform.openai.com/docs/api-reference/completions). */
static constexpr const char *kTextCompletion = "text_completion";
}  // namespace GenAiOperationNameValues

namespace GenAiSystemValues
{
/** OpenAI. */
static constexpr const char *kOpenai = "openai";
/** Vertex AI. */
static constexpr const char *kVertexAi = "vertex_ai";
/** Anthropic. */
static constexpr const char *kAnthropic = "anthropic";
/** Cohere. */
static constexpr const char *kCohere = "cohere";
}  // namespace GenAiSystemValues

namespace GenAiTokenTypeValues
{
/** Input tokens (prompt, input, etc.). */
static constexpr const char *kInput = "input";
/** Output tokens (completion, response, etc.). */
static constexpr const char *kCompletion = "output";
}  // namespace GenAiTokenTypeValues

namespace GoMemoryTypeValues
{
/** Memory allocated from the heap that is reserved for stack space, whether or not it is currently
 * in-use. */
static constexpr const char *kStack = "stack";
/** Memory used by the Go runtime, excluding other categories of memory usage described in this
 * enumeration. */
static constexpr const char *kOther = "other";
}  // namespace GoMemoryTypeValues

namespace GraphqlOperationTypeValues
{
/** GraphQL query. */
static constexpr const char *kQuery = "query";
/** GraphQL mutation. */
static constexpr const char *kMutation = "mutation";
/** GraphQL subscription. */
static constexpr const char *kSubscription = "subscription";
}  // namespace GraphqlOperationTypeValues

namespace HostArchValues
{
/** AMD64. */
static constexpr const char *kAmd64 = "amd64";
/** ARM32. */
static constexpr const char *kArm32 = "arm32";
/** ARM64. */
static constexpr const char *kArm64 = "arm64";
/** Itanium. */
static constexpr const char *kIa64 = "ia64";
/** 32-bit PowerPC. */
static constexpr const char *kPpc32 = "ppc32";
/** 64-bit PowerPC. */
static constexpr const char *kPpc64 = "ppc64";
/** IBM z/Architecture. */
static constexpr const char *kS390x = "s390x";
/** 32-bit x86. */
static constexpr const char *kX86 = "x86";
}  // namespace HostArchValues

namespace HttpConnectionStateValues
{
/** active state. */
static constexpr const char *kActive = "active";
/** idle state. */
static constexpr const char *kIdle = "idle";
}  // namespace HttpConnectionStateValues

namespace HttpRequestMethodValues
{
/** CONNECT method. */
static constexpr const char *kConnect = "CONNECT";
/** DELETE method. */
static constexpr const char *kDelete = "DELETE";
/** GET method. */
static constexpr const char *kGet = "GET";
/** HEAD method. */
static constexpr const char *kHead = "HEAD";
/** OPTIONS method. */
static constexpr const char *kOptions = "OPTIONS";
/** PATCH method. */
static constexpr const char *kPatch = "PATCH";
/** POST method. */
static constexpr const char *kPost = "POST";
/** PUT method. */
static constexpr const char *kPut = "PUT";
/** TRACE method. */
static constexpr const char *kTrace = "TRACE";
/** Any HTTP method that the instrumentation has no prior knowledge of. */
static constexpr const char *kOther = "_OTHER";
}  // namespace HttpRequestMethodValues

namespace JvmMemoryTypeValues
{
/** Heap memory. */
static constexpr const char *kHeap = "heap";
/** Non-heap memory. */
static constexpr const char *kNonHeap = "non_heap";
}  // namespace JvmMemoryTypeValues

namespace JvmThreadStateValues
{
/** A thread that has not yet started is in this state. */
static constexpr const char *kNew = "new";
/** A thread executing in the Java virtual machine is in this state. */
static constexpr const char *kRunnable = "runnable";
/** A thread that is blocked waiting for a monitor lock is in this state. */
static constexpr const char *kBlocked = "blocked";
/** A thread that is waiting indefinitely for another thread to perform a particular action is in
 * this state. */
static constexpr const char *kWaiting = "waiting";
/** A thread that is waiting for another thread to perform an action for up to a specified waiting
 * time is in this state. */
static constexpr const char *kTimedWaiting = "timed_waiting";
/** A thread that has exited is in this state. */
static constexpr const char *kTerminated = "terminated";
}  // namespace JvmThreadStateValues

namespace LinuxMemorySlabStateValues
{
/** reclaimable. */
static constexpr const char *kReclaimable = "reclaimable";
/** unreclaimable. */
static constexpr const char *kUnreclaimable = "unreclaimable";
}  // namespace LinuxMemorySlabStateValues

namespace LogIostreamValues
{
/** Logs from stdout stream. */
static constexpr const char *kStdout = "stdout";
/** Events from stderr stream. */
static constexpr const char *kStderr = "stderr";
}  // namespace LogIostreamValues

namespace MessagingOperationTypeValues
{
/** One or more messages are provided for publishing to an intermediary. If a single message is
 * published, the context of the &#34;Publish&#34; span can be used as the creation context and no
 * &#34;Create&#34; span needs to be created. */
static constexpr const char *kPublish = "publish";
/** A message is created. &#34;Create&#34; spans always refer to a single message and are used to
 * provide a unique creation context for messages in batch publishing scenarios. */
static constexpr const char *kCreate = "create";
/** One or more messages are requested by a consumer. This operation refers to pull-based scenarios,
 * where consumers explicitly call methods of messaging SDKs to receive messages. */
static constexpr const char *kReceive = "receive";
/** One or more messages are processed by a consumer. */
static constexpr const char *kProcess = "process";
/** One or more messages are settled. */
static constexpr const char *kSettle = "settle";
/** Deprecated. Use `process` instead. */
static constexpr const char *kDeliver = "deliver";
}  // namespace MessagingOperationTypeValues

namespace MessagingSystemValues
{
/** Apache ActiveMQ. */
static constexpr const char *kActivemq = "activemq";
/** Amazon Simple Queue Service (SQS). */
static constexpr const char *kAwsSqs = "aws_sqs";
/** Azure Event Grid. */
static constexpr const char *kEventgrid = "eventgrid";
/** Azure Event Hubs. */
static constexpr const char *kEventhubs = "eventhubs";
/** Azure Service Bus. */
static constexpr const char *kServicebus = "servicebus";
/** Google Cloud Pub/Sub. */
static constexpr const char *kGcpPubsub = "gcp_pubsub";
/** Java Message Service. */
static constexpr const char *kJms = "jms";
/** Apache Kafka. */
static constexpr const char *kKafka = "kafka";
/** RabbitMQ. */
static constexpr const char *kRabbitmq = "rabbitmq";
/** Apache RocketMQ. */
static constexpr const char *kRocketmq = "rocketmq";
/** Apache Pulsar. */
static constexpr const char *kPulsar = "pulsar";
}  // namespace MessagingSystemValues

namespace MessagingRocketmqConsumptionModelValues
{
/** Clustering consumption model. */
static constexpr const char *kClustering = "clustering";
/** Broadcasting consumption model. */
static constexpr const char *kBroadcasting = "broadcasting";
}  // namespace MessagingRocketmqConsumptionModelValues

namespace MessagingRocketmqMessageTypeValues
{
/** Normal message. */
static constexpr const char *kNormal = "normal";
/** FIFO message. */
static constexpr const char *kFifo = "fifo";
/** Delay message. */
static constexpr const char *kDelay = "delay";
/** Transaction message. */
static constexpr const char *kTransaction = "transaction";
}  // namespace MessagingRocketmqMessageTypeValues

namespace MessagingServicebusDispositionStatusValues
{
/** Message is completed. */
static constexpr const char *kComplete = "complete";
/** Message is abandoned. */
static constexpr const char *kAbandon = "abandon";
/** Message is sent to dead letter queue. */
static constexpr const char *kDeadLetter = "dead_letter";
/** Message is deferred. */
static constexpr const char *kDefer = "defer";
}  // namespace MessagingServicebusDispositionStatusValues

namespace NetworkConnectionSubtypeValues
{
/** GPRS. */
static constexpr const char *kGprs = "gprs";
/** EDGE. */
static constexpr const char *kEdge = "edge";
/** UMTS. */
static constexpr const char *kUmts = "umts";
/** CDMA. */
static constexpr const char *kCdma = "cdma";
/** EVDO Rel. 0. */
static constexpr const char *kEvdo0 = "evdo_0";
/** EVDO Rev. A. */
static constexpr const char *kEvdoA = "evdo_a";
/** CDMA2000 1XRTT. */
static constexpr const char *kCdma20001xrtt = "cdma2000_1xrtt";
/** HSDPA. */
static constexpr const char *kHsdpa = "hsdpa";
/** HSUPA. */
static constexpr const char *kHsupa = "hsupa";
/** HSPA. */
static constexpr const char *kHspa = "hspa";
/** IDEN. */
static constexpr const char *kIden = "iden";
/** EVDO Rev. B. */
static constexpr const char *kEvdoB = "evdo_b";
/** LTE. */
static constexpr const char *kLte = "lte";
/** EHRPD. */
static constexpr const char *kEhrpd = "ehrpd";
/** HSPAP. */
static constexpr const char *kHspap = "hspap";
/** GSM. */
static constexpr const char *kGsm = "gsm";
/** TD-SCDMA. */
static constexpr const char *kTdScdma = "td_scdma";
/** IWLAN. */
static constexpr const char *kIwlan = "iwlan";
/** 5G NR (New Radio). */
static constexpr const char *kNr = "nr";
/** 5G NRNSA (New Radio Non-Standalone). */
static constexpr const char *kNrnsa = "nrnsa";
/** LTE CA. */
static constexpr const char *kLteCa = "lte_ca";
}  // namespace NetworkConnectionSubtypeValues

namespace NetworkConnectionTypeValues
{
/** wifi. */
static constexpr const char *kWifi = "wifi";
/** wired. */
static constexpr const char *kWired = "wired";
/** cell. */
static constexpr const char *kCell = "cell";
/** unavailable. */
static constexpr const char *kUnavailable = "unavailable";
/** unknown. */
static constexpr const char *kUnknown = "unknown";
}  // namespace NetworkConnectionTypeValues

namespace NetworkIoDirectionValues
{
/** transmit. */
static constexpr const char *kTransmit = "transmit";
/** receive. */
static constexpr const char *kReceive = "receive";
}  // namespace NetworkIoDirectionValues

namespace NetworkTransportValues
{
/** TCP. */
static constexpr const char *kTcp = "tcp";
/** UDP. */
static constexpr const char *kUdp = "udp";
/** Named or anonymous pipe. */
static constexpr const char *kPipe = "pipe";
/** Unix domain socket. */
static constexpr const char *kUnix = "unix";
/** QUIC. */
static constexpr const char *kQuic = "quic";
}  // namespace NetworkTransportValues

namespace NetworkTypeValues
{
/** IPv4. */
static constexpr const char *kIpv4 = "ipv4";
/** IPv6. */
static constexpr const char *kIpv6 = "ipv6";
}  // namespace NetworkTypeValues

namespace OpentracingRefTypeValues
{
/** The parent Span depends on the child Span in some capacity. */
static constexpr const char *kChildOf = "child_of";
/** The parent Span doesn&#39;t depend in any way on the result of the child Span. */
static constexpr const char *kFollowsFrom = "follows_from";
}  // namespace OpentracingRefTypeValues

namespace OsTypeValues
{
/** Microsoft Windows. */
static constexpr const char *kWindows = "windows";
/** Linux. */
static constexpr const char *kLinux = "linux";
/** Apple Darwin. */
static constexpr const char *kDarwin = "darwin";
/** FreeBSD. */
static constexpr const char *kFreebsd = "freebsd";
/** NetBSD. */
static constexpr const char *kNetbsd = "netbsd";
/** OpenBSD. */
static constexpr const char *kOpenbsd = "openbsd";
/** DragonFly BSD. */
static constexpr const char *kDragonflybsd = "dragonflybsd";
/** HP-UX (Hewlett Packard Unix). */
static constexpr const char *kHpux = "hpux";
/** AIX (Advanced Interactive eXecutive). */
static constexpr const char *kAix = "aix";
/** SunOS, Oracle Solaris. */
static constexpr const char *kSolaris = "solaris";
/** IBM z/OS. */
static constexpr const char *kZOs = "z_os";
}  // namespace OsTypeValues

namespace OtelStatusCodeValues
{
/** The operation has been validated by an Application developer or Operator to have completed
 * successfully. */
static constexpr const char *kOk = "OK";
/** The operation contains an error. */
static constexpr const char *kError = "ERROR";
}  // namespace OtelStatusCodeValues

namespace ProcessContextSwitchTypeValues
{
/** voluntary. */
static constexpr const char *kVoluntary = "voluntary";
/** involuntary. */
static constexpr const char *kInvoluntary = "involuntary";
}  // namespace ProcessContextSwitchTypeValues

namespace ProcessPagingFaultTypeValues
{
/** major. */
static constexpr const char *kMajor = "major";
/** minor. */
static constexpr const char *kMinor = "minor";
}  // namespace ProcessPagingFaultTypeValues

namespace RpcConnectRpcErrorCodeValues
{
/** cancelled. */
static constexpr const char *kCancelled = "cancelled";
/** unknown. */
static constexpr const char *kUnknown = "unknown";
/** invalid_argument. */
static constexpr const char *kInvalidArgument = "invalid_argument";
/** deadline_exceeded. */
static constexpr const char *kDeadlineExceeded = "deadline_exceeded";
/** not_found. */
static constexpr const char *kNotFound = "not_found";
/** already_exists. */
static constexpr const char *kAlreadyExists = "already_exists";
/** permission_denied. */
static constexpr const char *kPermissionDenied = "permission_denied";
/** resource_exhausted. */
static constexpr const char *kResourceExhausted = "resource_exhausted";
/** failed_precondition. */
static constexpr const char *kFailedPrecondition = "failed_precondition";
/** aborted. */
static constexpr const char *kAborted = "aborted";
/** out_of_range. */
static constexpr const char *kOutOfRange = "out_of_range";
/** unimplemented. */
static constexpr const char *kUnimplemented = "unimplemented";
/** internal. */
static constexpr const char *kInternal = "internal";
/** unavailable. */
static constexpr const char *kUnavailable = "unavailable";
/** data_loss. */
static constexpr const char *kDataLoss = "data_loss";
/** unauthenticated. */
static constexpr const char *kUnauthenticated = "unauthenticated";
}  // namespace RpcConnectRpcErrorCodeValues

namespace RpcGrpcStatusCodeValues
{
/** OK. */
static constexpr const int kOk = 0;
/** CANCELLED. */
static constexpr const int kCancelled = 1;
/** UNKNOWN. */
static constexpr const int kUnknown = 2;
/** INVALID_ARGUMENT. */
static constexpr const int kInvalidArgument = 3;
/** DEADLINE_EXCEEDED. */
static constexpr const int kDeadlineExceeded = 4;
/** NOT_FOUND. */
static constexpr const int kNotFound = 5;
/** ALREADY_EXISTS. */
static constexpr const int kAlreadyExists = 6;
/** PERMISSION_DENIED. */
static constexpr const int kPermissionDenied = 7;
/** RESOURCE_EXHAUSTED. */
static constexpr const int kResourceExhausted = 8;
/** FAILED_PRECONDITION. */
static constexpr const int kFailedPrecondition = 9;
/** ABORTED. */
static constexpr const int kAborted = 10;
/** OUT_OF_RANGE. */
static constexpr const int kOutOfRange = 11;
/** UNIMPLEMENTED. */
static constexpr const int kUnimplemented = 12;
/** INTERNAL. */
static constexpr const int kInternal = 13;
/** UNAVAILABLE. */
static constexpr const int kUnavailable = 14;
/** DATA_LOSS. */
static constexpr const int kDataLoss = 15;
/** UNAUTHENTICATED. */
static constexpr const int kUnauthenticated = 16;
}  // namespace RpcGrpcStatusCodeValues

namespace RpcMessageTypeValues
{
/** sent. */
static constexpr const char *kSent = "SENT";
/** received. */
static constexpr const char *kReceived = "RECEIVED";
}  // namespace RpcMessageTypeValues

namespace RpcSystemValues
{
/** gRPC. */
static constexpr const char *kGrpc = "grpc";
/** Java RMI. */
static constexpr const char *kJavaRmi = "java_rmi";
/** .NET WCF. */
static constexpr const char *kDotnetWcf = "dotnet_wcf";
/** Apache Dubbo. */
static constexpr const char *kApacheDubbo = "apache_dubbo";
/** Connect RPC. */
static constexpr const char *kConnectRpc = "connect_rpc";
}  // namespace RpcSystemValues

namespace SignalrConnectionStatusValues
{
/** The connection was closed normally. */
static constexpr const char *kNormalClosure = "normal_closure";
/** The connection was closed due to a timeout. */
static constexpr const char *kTimeout = "timeout";
/** The connection was closed because the app is shutting down. */
static constexpr const char *kAppShutdown = "app_shutdown";
}  // namespace SignalrConnectionStatusValues

namespace SignalrTransportValues
{
/** ServerSentEvents protocol. */
static constexpr const char *kServerSentEvents = "server_sent_events";
/** LongPolling protocol. */
static constexpr const char *kLongPolling = "long_polling";
/** WebSockets protocol. */
static constexpr const char *kWebSockets = "web_sockets";
}  // namespace SignalrTransportValues

namespace SystemMemoryStateValues
{
/** used. */
static constexpr const char *kUsed = "used";
/** free. */
static constexpr const char *kFree = "free";
/** shared. */
static constexpr const char *kShared = "shared";
/** buffers. */
static constexpr const char *kBuffers = "buffers";
/** cached. */
static constexpr const char *kCached = "cached";
}  // namespace SystemMemoryStateValues

namespace SystemPagingDirectionValues
{
/** in. */
static constexpr const char *kIn = "in";
/** out. */
static constexpr const char *kOut = "out";
}  // namespace SystemPagingDirectionValues

namespace SystemPagingStateValues
{
/** used. */
static constexpr const char *kUsed = "used";
/** free. */
static constexpr const char *kFree = "free";
}  // namespace SystemPagingStateValues

namespace SystemPagingTypeValues
{
/** major. */
static constexpr const char *kMajor = "major";
/** minor. */
static constexpr const char *kMinor = "minor";
}  // namespace SystemPagingTypeValues

namespace SystemFilesystemStateValues
{
/** used. */
static constexpr const char *kUsed = "used";
/** free. */
static constexpr const char *kFree = "free";
/** reserved. */
static constexpr const char *kReserved = "reserved";
}  // namespace SystemFilesystemStateValues

namespace SystemFilesystemTypeValues
{
/** fat32. */
static constexpr const char *kFat32 = "fat32";
/** exfat. */
static constexpr const char *kExfat = "exfat";
/** ntfs. */
static constexpr const char *kNtfs = "ntfs";
/** refs. */
static constexpr const char *kRefs = "refs";
/** hfsplus. */
static constexpr const char *kHfsplus = "hfsplus";
/** ext4. */
static constexpr const char *kExt4 = "ext4";
}  // namespace SystemFilesystemTypeValues

namespace SystemNetworkStateValues
{
/** close. */
static constexpr const char *kClose = "close";
/** close_wait. */
static constexpr const char *kCloseWait = "close_wait";
/** closing. */
static constexpr const char *kClosing = "closing";
/** delete. */
static constexpr const char *kDelete = "delete";
/** established. */
static constexpr const char *kEstablished = "established";
/** fin_wait_1. */
static constexpr const char *kFinWait1 = "fin_wait_1";
/** fin_wait_2. */
static constexpr const char *kFinWait2 = "fin_wait_2";
/** last_ack. */
static constexpr const char *kLastAck = "last_ack";
/** listen. */
static constexpr const char *kListen = "listen";
/** syn_recv. */
static constexpr const char *kSynRecv = "syn_recv";
/** syn_sent. */
static constexpr const char *kSynSent = "syn_sent";
/** time_wait. */
static constexpr const char *kTimeWait = "time_wait";
}  // namespace SystemNetworkStateValues

namespace SystemProcessStatusValues
{
/** running. */
static constexpr const char *kRunning = "running";
/** sleeping. */
static constexpr const char *kSleeping = "sleeping";
/** stopped. */
static constexpr const char *kStopped = "stopped";
/** defunct. */
static constexpr const char *kDefunct = "defunct";
}  // namespace SystemProcessStatusValues

namespace TelemetrySdkLanguageValues
{
/** cpp. */
static constexpr const char *kCpp = "cpp";
/** dotnet. */
static constexpr const char *kDotnet = "dotnet";
/** erlang. */
static constexpr const char *kErlang = "erlang";
/** go. */
static constexpr const char *kGo = "go";
/** java. */
static constexpr const char *kJava = "java";
/** nodejs. */
static constexpr const char *kNodejs = "nodejs";
/** php. */
static constexpr const char *kPhp = "php";
/** python. */
static constexpr const char *kPython = "python";
/** ruby. */
static constexpr const char *kRuby = "ruby";
/** rust. */
static constexpr const char *kRust = "rust";
/** swift. */
static constexpr const char *kSwift = "swift";
/** webjs. */
static constexpr const char *kWebjs = "webjs";
}  // namespace TelemetrySdkLanguageValues

namespace TestCaseResultStatusValues
{
/** pass. */
static constexpr const char *kPass = "pass";
/** fail. */
static constexpr const char *kFail = "fail";
}  // namespace TestCaseResultStatusValues

namespace TestSuiteRunStatusValues
{
/** success. */
static constexpr const char *kSuccess = "success";
/** failure. */
static constexpr const char *kFailure = "failure";
/** skipped. */
static constexpr const char *kSkipped = "skipped";
/** aborted. */
static constexpr const char *kAborted = "aborted";
/** timed_out. */
static constexpr const char *kTimedOut = "timed_out";
/** in_progress. */
static constexpr const char *kInProgress = "in_progress";
}  // namespace TestSuiteRunStatusValues

namespace TlsProtocolNameValues
{
/** ssl. */
static constexpr const char *kSsl = "ssl";
/** tls. */
static constexpr const char *kTls = "tls";
}  // namespace TlsProtocolNameValues

namespace V8jsGcTypeValues
{
/** Major (Mark Sweep Compact). */
static constexpr const char *kMajor = "major";
/** Minor (Scavenge). */
static constexpr const char *kMinor = "minor";
/** Incremental (Incremental Marking). */
static constexpr const char *kIncremental = "incremental";
/** Weak Callbacks (Process Weak Callbacks). */
static constexpr const char *kWeakcb = "weakcb";
}  // namespace V8jsGcTypeValues

namespace V8jsHeapSpaceNameValues
{
/** New memory space. */
static constexpr const char *kNewSpace = "new_space";
/** Old memory space. */
static constexpr const char *kOldSpace = "old_space";
/** Code memory space. */
static constexpr const char *kCodeSpace = "code_space";
/** Map memory space. */
static constexpr const char *kMapSpace = "map_space";
/** Large object memory space. */
static constexpr const char *kLargeObjectSpace = "large_object_space";
}  // namespace V8jsHeapSpaceNameValues

namespace VcsRepositoryRefTypeValues
{
/** [branch](https://git-scm.com/docs/gitglossary#Documentation/gitglossary.txt-aiddefbranchabranch). */
static constexpr const char *kBranch = "branch";
/** [tag](https://git-scm.com/docs/gitglossary#Documentation/gitglossary.txt-aiddeftagatag). */
static constexpr const char *kTag = "tag";
}  // namespace VcsRepositoryRefTypeValues

}  // namespace SemanticConventions
}  // namespace trace
OPENTELEMETRY_END_NAMESPACE
