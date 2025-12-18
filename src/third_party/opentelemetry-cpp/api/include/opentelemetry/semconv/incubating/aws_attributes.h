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
namespace aws
{

/**
  The unique identifier of the AWS Bedrock Guardrail. A <a
  href="https://docs.aws.amazon.com/bedrock/latest/userguide/guardrails.html">guardrail</a> helps
  safeguard and prevent unwanted behavior from model responses or user messages.
 */
static constexpr const char *kAwsBedrockGuardrailId = "aws.bedrock.guardrail.id";

/**
  The unique identifier of the AWS Bedrock Knowledge base. A <a
  href="https://docs.aws.amazon.com/bedrock/latest/userguide/knowledge-base.html">knowledge base</a>
  is a bank of information that can be queried by models to generate more relevant responses and
  augment prompts.
 */
static constexpr const char *kAwsBedrockKnowledgeBaseId = "aws.bedrock.knowledge_base.id";

/**
  The JSON-serialized value of each item in the @code AttributeDefinitions @endcode request field.
 */
static constexpr const char *kAwsDynamodbAttributeDefinitions =
    "aws.dynamodb.attribute_definitions";

/**
  The value of the @code AttributesToGet @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbAttributesToGet = "aws.dynamodb.attributes_to_get";

/**
  The value of the @code ConsistentRead @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbConsistentRead = "aws.dynamodb.consistent_read";

/**
  The JSON-serialized value of each item in the @code ConsumedCapacity @endcode response field.
 */
static constexpr const char *kAwsDynamodbConsumedCapacity = "aws.dynamodb.consumed_capacity";

/**
  The value of the @code Count @endcode response parameter.
 */
static constexpr const char *kAwsDynamodbCount = "aws.dynamodb.count";

/**
  The value of the @code ExclusiveStartTableName @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbExclusiveStartTable = "aws.dynamodb.exclusive_start_table";

/**
  The JSON-serialized value of each item in the @code GlobalSecondaryIndexUpdates @endcode request
  field.
 */
static constexpr const char *kAwsDynamodbGlobalSecondaryIndexUpdates =
    "aws.dynamodb.global_secondary_index_updates";

/**
  The JSON-serialized value of each item of the @code GlobalSecondaryIndexes @endcode request field
 */
static constexpr const char *kAwsDynamodbGlobalSecondaryIndexes =
    "aws.dynamodb.global_secondary_indexes";

/**
  The value of the @code IndexName @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbIndexName = "aws.dynamodb.index_name";

/**
  The JSON-serialized value of the @code ItemCollectionMetrics @endcode response field.
 */
static constexpr const char *kAwsDynamodbItemCollectionMetrics =
    "aws.dynamodb.item_collection_metrics";

/**
  The value of the @code Limit @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbLimit = "aws.dynamodb.limit";

/**
  The JSON-serialized value of each item of the @code LocalSecondaryIndexes @endcode request field.
 */
static constexpr const char *kAwsDynamodbLocalSecondaryIndexes =
    "aws.dynamodb.local_secondary_indexes";

/**
  The value of the @code ProjectionExpression @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbProjection = "aws.dynamodb.projection";

/**
  The value of the @code ProvisionedThroughput.ReadCapacityUnits @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbProvisionedReadCapacity =
    "aws.dynamodb.provisioned_read_capacity";

/**
  The value of the @code ProvisionedThroughput.WriteCapacityUnits @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbProvisionedWriteCapacity =
    "aws.dynamodb.provisioned_write_capacity";

/**
  The value of the @code ScanIndexForward @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbScanForward = "aws.dynamodb.scan_forward";

/**
  The value of the @code ScannedCount @endcode response parameter.
 */
static constexpr const char *kAwsDynamodbScannedCount = "aws.dynamodb.scanned_count";

/**
  The value of the @code Segment @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbSegment = "aws.dynamodb.segment";

/**
  The value of the @code Select @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbSelect = "aws.dynamodb.select";

/**
  The number of items in the @code TableNames @endcode response parameter.
 */
static constexpr const char *kAwsDynamodbTableCount = "aws.dynamodb.table_count";

/**
  The keys in the @code RequestItems @endcode object field.
 */
static constexpr const char *kAwsDynamodbTableNames = "aws.dynamodb.table_names";

/**
  The value of the @code TotalSegments @endcode request parameter.
 */
static constexpr const char *kAwsDynamodbTotalSegments = "aws.dynamodb.total_segments";

/**
  The ARN of an <a
  href="https://docs.aws.amazon.com/AmazonECS/latest/developerguide/clusters.html">ECS cluster</a>.
 */
static constexpr const char *kAwsEcsClusterArn = "aws.ecs.cluster.arn";

/**
  The Amazon Resource Name (ARN) of an <a
  href="https://docs.aws.amazon.com/AmazonECS/latest/developerguide/ECS_instances.html">ECS
  container instance</a>.
 */
static constexpr const char *kAwsEcsContainerArn = "aws.ecs.container.arn";

/**
  The <a href="https://docs.aws.amazon.com/AmazonECS/latest/developerguide/launch_types.html">launch
  type</a> for an ECS task.
 */
static constexpr const char *kAwsEcsLaunchtype = "aws.ecs.launchtype";

/**
  The ARN of a running <a
  href="https://docs.aws.amazon.com/AmazonECS/latest/developerguide/ecs-account-settings.html#ecs-resource-ids">ECS
  task</a>.
 */
static constexpr const char *kAwsEcsTaskArn = "aws.ecs.task.arn";

/**
  The family name of the <a
  href="https://docs.aws.amazon.com/AmazonECS/latest/developerguide/task_definitions.html">ECS task
  definition</a> used to create the ECS task.
 */
static constexpr const char *kAwsEcsTaskFamily = "aws.ecs.task.family";

/**
  The ID of a running ECS task. The ID MUST be extracted from @code task.arn @endcode.
 */
static constexpr const char *kAwsEcsTaskId = "aws.ecs.task.id";

/**
  The revision for the task definition used to create the ECS task.
 */
static constexpr const char *kAwsEcsTaskRevision = "aws.ecs.task.revision";

/**
  The ARN of an EKS cluster.
 */
static constexpr const char *kAwsEksClusterArn = "aws.eks.cluster.arn";

/**
  The AWS extended request ID as returned in the response header @code x-amz-id-2 @endcode.
 */
static constexpr const char *kAwsExtendedRequestId = "aws.extended_request_id";

/**
  The name of the AWS Kinesis <a
  href="https://docs.aws.amazon.com/streams/latest/dev/introduction.html">stream</a> the request
  refers to. Corresponds to the @code --stream-name @endcode parameter of the Kinesis <a
  href="https://docs.aws.amazon.com/cli/latest/reference/kinesis/describe-stream.html">describe-stream</a>
  operation.
 */
static constexpr const char *kAwsKinesisStreamName = "aws.kinesis.stream_name";

/**
  The full invoked ARN as provided on the @code Context @endcode passed to the function (@code
  Lambda-Runtime-Invoked-Function-Arn @endcode header on the @code /runtime/invocation/next @endcode
  applicable). <p> This may be different from @code cloud.resource_id @endcode if an alias is
  involved.
 */
static constexpr const char *kAwsLambdaInvokedArn = "aws.lambda.invoked_arn";

/**
  The UUID of the <a
  href="https://docs.aws.amazon.com/AWSCloudFormation/latest/UserGuide/aws-resource-lambda-eventsourcemapping.html">AWS
  Lambda EvenSource Mapping</a>. An event source is mapped to a lambda function. It's contents are
  read by Lambda and used to trigger a function. This isn't available in the lambda execution
  context or the lambda runtime environtment. This is going to be populated by the AWS SDK for each
  language when that UUID is present. Some of these operations are Create/Delete/Get/List/Update
  EventSourceMapping.
 */
static constexpr const char *kAwsLambdaResourceMappingId = "aws.lambda.resource_mapping.id";

/**
  The Amazon Resource Name(s) (ARN) of the AWS log group(s).
  <p>
  See the <a
  href="https://docs.aws.amazon.com/AmazonCloudWatch/latest/logs/iam-access-control-overview-cwl.html#CWL_ARN_Format">log
  group ARN format documentation</a>.
 */
static constexpr const char *kAwsLogGroupArns = "aws.log.group.arns";

/**
  The name(s) of the AWS log group(s) an application is writing to.
  <p>
  Multiple log groups must be supported for cases like multi-container applications, where a single
  application has sidecar containers, and each write to their own log group.
 */
static constexpr const char *kAwsLogGroupNames = "aws.log.group.names";

/**
  The ARN(s) of the AWS log stream(s).
  <p>
  See the <a
  href="https://docs.aws.amazon.com/AmazonCloudWatch/latest/logs/iam-access-control-overview-cwl.html#CWL_ARN_Format">log
  stream ARN format documentation</a>. One log group can contain several log streams, so these ARNs
  necessarily identify both a log group and a log stream.
 */
static constexpr const char *kAwsLogStreamArns = "aws.log.stream.arns";

/**
  The name(s) of the AWS log stream(s) an application is writing to.
 */
static constexpr const char *kAwsLogStreamNames = "aws.log.stream.names";

/**
  The AWS request ID as returned in the response headers @code x-amzn-requestid @endcode, @code
  x-amzn-request-id @endcode or @code x-amz-request-id @endcode.
 */
static constexpr const char *kAwsRequestId = "aws.request_id";

/**
  The S3 bucket name the request refers to. Corresponds to the @code --bucket @endcode parameter of
  the <a href="https://docs.aws.amazon.com/cli/latest/reference/s3api/index.html">S3 API</a>
  operations. <p> The @code bucket @endcode attribute is applicable to all S3 operations that
  reference a bucket, i.e. that require the bucket name as a mandatory parameter. This applies to
  almost all S3 operations except @code list-buckets @endcode.
 */
static constexpr const char *kAwsS3Bucket = "aws.s3.bucket";

/**
  The source object (in the form @code bucket @endcode/@code key @endcode) for the copy operation.
  <p>
  The @code copy_source @endcode attribute applies to S3 copy operations and corresponds to the
  @code --copy-source @endcode parameter of the <a
  href="https://docs.aws.amazon.com/cli/latest/reference/s3api/copy-object.html">copy-object
  operation within the S3 API</a>. This applies in particular to the following operations: <ul>
    <li><a
  href="https://docs.aws.amazon.com/cli/latest/reference/s3api/copy-object.html">copy-object</a></li>
    <li><a
  href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part-copy.html">upload-part-copy</a></li>
  </ul>
 */
static constexpr const char *kAwsS3CopySource = "aws.s3.copy_source";

/**
  The delete request container that specifies the objects to be deleted.
  <p>
  The @code delete @endcode attribute is only applicable to the <a
  href="https://docs.aws.amazon.com/cli/latest/reference/s3api/delete-object.html">delete-object</a>
  operation. The @code delete @endcode attribute corresponds to the @code --delete @endcode
  parameter of the <a
  href="https://docs.aws.amazon.com/cli/latest/reference/s3api/delete-objects.html">delete-objects
  operation within the S3 API</a>.
 */
static constexpr const char *kAwsS3Delete = "aws.s3.delete";

/**
  The S3 object key the request refers to. Corresponds to the @code --key @endcode parameter of the
  <a href="https://docs.aws.amazon.com/cli/latest/reference/s3api/index.html">S3 API</a> operations.
  <p>
  The @code key @endcode attribute is applicable to all object-related S3 operations, i.e. that
  require the object key as a mandatory parameter. This applies in particular to the following
  operations: <ul> <li><a
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
  The part number of the part being uploaded in a multipart-upload operation. This is a positive
  integer between 1 and 10,000. <p> The @code part_number @endcode attribute is only applicable to
  the <a
  href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part.html">upload-part</a> and
  <a
  href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part-copy.html">upload-part-copy</a>
  operations. The @code part_number @endcode attribute corresponds to the @code --part-number
  @endcode parameter of the <a
  href="https://docs.aws.amazon.com/cli/latest/reference/s3api/upload-part.html">upload-part
  operation within the S3 API</a>.
 */
static constexpr const char *kAwsS3PartNumber = "aws.s3.part_number";

/**
  Upload ID that identifies the multipart upload.
  <p>
  The @code upload_id @endcode attribute applies to S3 multipart-upload operations and corresponds
  to the @code --upload-id @endcode parameter of the <a
  href="https://docs.aws.amazon.com/cli/latest/reference/s3api/index.html">S3 API</a> multipart
  operations. This applies in particular to the following operations: <ul> <li><a
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
  The ARN of the Secret stored in the Secrets Mangger
 */
static constexpr const char *kAwsSecretsmanagerSecretArn = "aws.secretsmanager.secret.arn";

/**
  The ARN of the AWS SNS Topic. An Amazon SNS <a
  href="https://docs.aws.amazon.com/sns/latest/dg/sns-create-topic.html">topic</a> is a logical
  access point that acts as a communication channel.
 */
static constexpr const char *kAwsSnsTopicArn = "aws.sns.topic.arn";

/**
  The URL of the AWS SQS Queue. It's a unique identifier for a queue in Amazon Simple Queue Service
  (SQS) and is used to access the queue and perform actions on it.
 */
static constexpr const char *kAwsSqsQueueUrl = "aws.sqs.queue.url";

/**
  The ARN of the AWS Step Functions Activity.
 */
static constexpr const char *kAwsStepFunctionsActivityArn = "aws.step_functions.activity.arn";

/**
  The ARN of the AWS Step Functions State Machine.
 */
static constexpr const char *kAwsStepFunctionsStateMachineArn =
    "aws.step_functions.state_machine.arn";

namespace AwsEcsLaunchtypeValues
{
/**
  Amazon EC2
 */
static constexpr const char *kEc2 = "ec2";

/**
  Amazon Fargate
 */
static constexpr const char *kFargate = "fargate";

}  // namespace AwsEcsLaunchtypeValues

}  // namespace aws
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
