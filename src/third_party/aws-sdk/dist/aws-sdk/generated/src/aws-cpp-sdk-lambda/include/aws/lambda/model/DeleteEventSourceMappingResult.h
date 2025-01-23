/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/EventSourcePosition.h>
#include <aws/core/utils/DateTime.h>
#include <aws/lambda/model/FilterCriteria.h>
#include <aws/lambda/model/DestinationConfig.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/SelfManagedEventSource.h>
#include <aws/lambda/model/AmazonManagedKafkaEventSourceConfig.h>
#include <aws/lambda/model/SelfManagedKafkaEventSourceConfig.h>
#include <aws/lambda/model/ScalingConfig.h>
#include <aws/lambda/model/DocumentDBEventSourceConfig.h>
#include <aws/lambda/model/FilterCriteriaError.h>
#include <aws/lambda/model/EventSourceMappingMetricsConfig.h>
#include <aws/lambda/model/ProvisionedPollerConfig.h>
#include <aws/lambda/model/SourceAccessConfiguration.h>
#include <aws/lambda/model/FunctionResponseType.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Json
{
  class JsonValue;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{
  /**
   * <p>A mapping between an Amazon Web Services resource and a Lambda function. For
   * details, see <a>CreateEventSourceMapping</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/EventSourceMappingConfiguration">AWS
   * API Reference</a></p>
   */
  class DeleteEventSourceMappingResult
  {
  public:
    AWS_LAMBDA_API DeleteEventSourceMappingResult();
    AWS_LAMBDA_API DeleteEventSourceMappingResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API DeleteEventSourceMappingResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The identifier of the event source mapping.</p>
     */
    inline const Aws::String& GetUUID() const{ return m_uUID; }
    inline void SetUUID(const Aws::String& value) { m_uUID = value; }
    inline void SetUUID(Aws::String&& value) { m_uUID = std::move(value); }
    inline void SetUUID(const char* value) { m_uUID.assign(value); }
    inline DeleteEventSourceMappingResult& WithUUID(const Aws::String& value) { SetUUID(value); return *this;}
    inline DeleteEventSourceMappingResult& WithUUID(Aws::String&& value) { SetUUID(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& WithUUID(const char* value) { SetUUID(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The position in a stream from which to start reading. Required for Amazon
     * Kinesis and Amazon DynamoDB Stream event sources. <code>AT_TIMESTAMP</code> is
     * supported only for Amazon Kinesis streams, Amazon DocumentDB, Amazon MSK, and
     * self-managed Apache Kafka.</p>
     */
    inline const EventSourcePosition& GetStartingPosition() const{ return m_startingPosition; }
    inline void SetStartingPosition(const EventSourcePosition& value) { m_startingPosition = value; }
    inline void SetStartingPosition(EventSourcePosition&& value) { m_startingPosition = std::move(value); }
    inline DeleteEventSourceMappingResult& WithStartingPosition(const EventSourcePosition& value) { SetStartingPosition(value); return *this;}
    inline DeleteEventSourceMappingResult& WithStartingPosition(EventSourcePosition&& value) { SetStartingPosition(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>With <code>StartingPosition</code> set to <code>AT_TIMESTAMP</code>, the time
     * from which to start reading. <code>StartingPositionTimestamp</code> cannot be in
     * the future.</p>
     */
    inline const Aws::Utils::DateTime& GetStartingPositionTimestamp() const{ return m_startingPositionTimestamp; }
    inline void SetStartingPositionTimestamp(const Aws::Utils::DateTime& value) { m_startingPositionTimestamp = value; }
    inline void SetStartingPositionTimestamp(Aws::Utils::DateTime&& value) { m_startingPositionTimestamp = std::move(value); }
    inline DeleteEventSourceMappingResult& WithStartingPositionTimestamp(const Aws::Utils::DateTime& value) { SetStartingPositionTimestamp(value); return *this;}
    inline DeleteEventSourceMappingResult& WithStartingPositionTimestamp(Aws::Utils::DateTime&& value) { SetStartingPositionTimestamp(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of records in each batch that Lambda pulls from your
     * stream or queue and sends to your function. Lambda passes all of the records in
     * the batch to the function in a single call, up to the payload limit for
     * synchronous invocation (6 MB).</p> <p>Default value: Varies by service. For
     * Amazon SQS, the default is 10. For all other services, the default is 100.</p>
     * <p>Related setting: When you set <code>BatchSize</code> to a value greater than
     * 10, you must set <code>MaximumBatchingWindowInSeconds</code> to at least 1.</p>
     */
    inline int GetBatchSize() const{ return m_batchSize; }
    inline void SetBatchSize(int value) { m_batchSize = value; }
    inline DeleteEventSourceMappingResult& WithBatchSize(int value) { SetBatchSize(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum amount of time, in seconds, that Lambda spends gathering records
     * before invoking the function. You can configure
     * <code>MaximumBatchingWindowInSeconds</code> to any value from 0 seconds to 300
     * seconds in increments of seconds.</p> <p>For streams and Amazon SQS event
     * sources, the default batching window is 0 seconds. For Amazon MSK, Self-managed
     * Apache Kafka, Amazon MQ, and DocumentDB event sources, the default batching
     * window is 500 ms. Note that because you can only change
     * <code>MaximumBatchingWindowInSeconds</code> in increments of seconds, you cannot
     * revert back to the 500 ms default batching window after you have changed it. To
     * restore the default batching window, you must create a new event source
     * mapping.</p> <p>Related setting: For streams and Amazon SQS event sources, when
     * you set <code>BatchSize</code> to a value greater than 10, you must set
     * <code>MaximumBatchingWindowInSeconds</code> to at least 1.</p>
     */
    inline int GetMaximumBatchingWindowInSeconds() const{ return m_maximumBatchingWindowInSeconds; }
    inline void SetMaximumBatchingWindowInSeconds(int value) { m_maximumBatchingWindowInSeconds = value; }
    inline DeleteEventSourceMappingResult& WithMaximumBatchingWindowInSeconds(int value) { SetMaximumBatchingWindowInSeconds(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>(Kinesis and DynamoDB Streams only) The number of batches to process
     * concurrently from each shard. The default value is 1.</p>
     */
    inline int GetParallelizationFactor() const{ return m_parallelizationFactor; }
    inline void SetParallelizationFactor(int value) { m_parallelizationFactor = value; }
    inline DeleteEventSourceMappingResult& WithParallelizationFactor(int value) { SetParallelizationFactor(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the event source.</p>
     */
    inline const Aws::String& GetEventSourceArn() const{ return m_eventSourceArn; }
    inline void SetEventSourceArn(const Aws::String& value) { m_eventSourceArn = value; }
    inline void SetEventSourceArn(Aws::String&& value) { m_eventSourceArn = std::move(value); }
    inline void SetEventSourceArn(const char* value) { m_eventSourceArn.assign(value); }
    inline DeleteEventSourceMappingResult& WithEventSourceArn(const Aws::String& value) { SetEventSourceArn(value); return *this;}
    inline DeleteEventSourceMappingResult& WithEventSourceArn(Aws::String&& value) { SetEventSourceArn(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& WithEventSourceArn(const char* value) { SetEventSourceArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An object that defines the filter criteria that determine whether Lambda
     * should process an event. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-eventfiltering.html">Lambda
     * event filtering</a>.</p> <p>If filter criteria is encrypted, this field shows up
     * as <code>null</code> in the response of ListEventSourceMapping API calls. You
     * can view this field in plaintext in the response of GetEventSourceMapping and
     * DeleteEventSourceMapping calls if you have <code>kms:Decrypt</code> permissions
     * for the correct KMS key.</p>
     */
    inline const FilterCriteria& GetFilterCriteria() const{ return m_filterCriteria; }
    inline void SetFilterCriteria(const FilterCriteria& value) { m_filterCriteria = value; }
    inline void SetFilterCriteria(FilterCriteria&& value) { m_filterCriteria = std::move(value); }
    inline DeleteEventSourceMappingResult& WithFilterCriteria(const FilterCriteria& value) { SetFilterCriteria(value); return *this;}
    inline DeleteEventSourceMappingResult& WithFilterCriteria(FilterCriteria&& value) { SetFilterCriteria(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the Lambda function.</p>
     */
    inline const Aws::String& GetFunctionArn() const{ return m_functionArn; }
    inline void SetFunctionArn(const Aws::String& value) { m_functionArn = value; }
    inline void SetFunctionArn(Aws::String&& value) { m_functionArn = std::move(value); }
    inline void SetFunctionArn(const char* value) { m_functionArn.assign(value); }
    inline DeleteEventSourceMappingResult& WithFunctionArn(const Aws::String& value) { SetFunctionArn(value); return *this;}
    inline DeleteEventSourceMappingResult& WithFunctionArn(Aws::String&& value) { SetFunctionArn(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& WithFunctionArn(const char* value) { SetFunctionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date that the event source mapping was last updated or that its state
     * changed.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModified() const{ return m_lastModified; }
    inline void SetLastModified(const Aws::Utils::DateTime& value) { m_lastModified = value; }
    inline void SetLastModified(Aws::Utils::DateTime&& value) { m_lastModified = std::move(value); }
    inline DeleteEventSourceMappingResult& WithLastModified(const Aws::Utils::DateTime& value) { SetLastModified(value); return *this;}
    inline DeleteEventSourceMappingResult& WithLastModified(Aws::Utils::DateTime&& value) { SetLastModified(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The result of the last Lambda invocation of your function.</p>
     */
    inline const Aws::String& GetLastProcessingResult() const{ return m_lastProcessingResult; }
    inline void SetLastProcessingResult(const Aws::String& value) { m_lastProcessingResult = value; }
    inline void SetLastProcessingResult(Aws::String&& value) { m_lastProcessingResult = std::move(value); }
    inline void SetLastProcessingResult(const char* value) { m_lastProcessingResult.assign(value); }
    inline DeleteEventSourceMappingResult& WithLastProcessingResult(const Aws::String& value) { SetLastProcessingResult(value); return *this;}
    inline DeleteEventSourceMappingResult& WithLastProcessingResult(Aws::String&& value) { SetLastProcessingResult(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& WithLastProcessingResult(const char* value) { SetLastProcessingResult(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The state of the event source mapping. It can be one of the following:
     * <code>Creating</code>, <code>Enabling</code>, <code>Enabled</code>,
     * <code>Disabling</code>, <code>Disabled</code>, <code>Updating</code>, or
     * <code>Deleting</code>.</p>
     */
    inline const Aws::String& GetState() const{ return m_state; }
    inline void SetState(const Aws::String& value) { m_state = value; }
    inline void SetState(Aws::String&& value) { m_state = std::move(value); }
    inline void SetState(const char* value) { m_state.assign(value); }
    inline DeleteEventSourceMappingResult& WithState(const Aws::String& value) { SetState(value); return *this;}
    inline DeleteEventSourceMappingResult& WithState(Aws::String&& value) { SetState(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& WithState(const char* value) { SetState(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether a user or Lambda made the last change to the event source
     * mapping.</p>
     */
    inline const Aws::String& GetStateTransitionReason() const{ return m_stateTransitionReason; }
    inline void SetStateTransitionReason(const Aws::String& value) { m_stateTransitionReason = value; }
    inline void SetStateTransitionReason(Aws::String&& value) { m_stateTransitionReason = std::move(value); }
    inline void SetStateTransitionReason(const char* value) { m_stateTransitionReason.assign(value); }
    inline DeleteEventSourceMappingResult& WithStateTransitionReason(const Aws::String& value) { SetStateTransitionReason(value); return *this;}
    inline DeleteEventSourceMappingResult& WithStateTransitionReason(Aws::String&& value) { SetStateTransitionReason(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& WithStateTransitionReason(const char* value) { SetStateTransitionReason(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>(Kinesis, DynamoDB Streams, Amazon MSK, and self-managed Apache Kafka event
     * sources only) A configuration object that specifies the destination of an event
     * after Lambda processes it.</p>
     */
    inline const DestinationConfig& GetDestinationConfig() const{ return m_destinationConfig; }
    inline void SetDestinationConfig(const DestinationConfig& value) { m_destinationConfig = value; }
    inline void SetDestinationConfig(DestinationConfig&& value) { m_destinationConfig = std::move(value); }
    inline DeleteEventSourceMappingResult& WithDestinationConfig(const DestinationConfig& value) { SetDestinationConfig(value); return *this;}
    inline DeleteEventSourceMappingResult& WithDestinationConfig(DestinationConfig&& value) { SetDestinationConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the Kafka topic.</p>
     */
    inline const Aws::Vector<Aws::String>& GetTopics() const{ return m_topics; }
    inline void SetTopics(const Aws::Vector<Aws::String>& value) { m_topics = value; }
    inline void SetTopics(Aws::Vector<Aws::String>&& value) { m_topics = std::move(value); }
    inline DeleteEventSourceMappingResult& WithTopics(const Aws::Vector<Aws::String>& value) { SetTopics(value); return *this;}
    inline DeleteEventSourceMappingResult& WithTopics(Aws::Vector<Aws::String>&& value) { SetTopics(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& AddTopics(const Aws::String& value) { m_topics.push_back(value); return *this; }
    inline DeleteEventSourceMappingResult& AddTopics(Aws::String&& value) { m_topics.push_back(std::move(value)); return *this; }
    inline DeleteEventSourceMappingResult& AddTopics(const char* value) { m_topics.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p> (Amazon MQ) The name of the Amazon MQ broker destination queue to
     * consume.</p>
     */
    inline const Aws::Vector<Aws::String>& GetQueues() const{ return m_queues; }
    inline void SetQueues(const Aws::Vector<Aws::String>& value) { m_queues = value; }
    inline void SetQueues(Aws::Vector<Aws::String>&& value) { m_queues = std::move(value); }
    inline DeleteEventSourceMappingResult& WithQueues(const Aws::Vector<Aws::String>& value) { SetQueues(value); return *this;}
    inline DeleteEventSourceMappingResult& WithQueues(Aws::Vector<Aws::String>&& value) { SetQueues(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& AddQueues(const Aws::String& value) { m_queues.push_back(value); return *this; }
    inline DeleteEventSourceMappingResult& AddQueues(Aws::String&& value) { m_queues.push_back(std::move(value)); return *this; }
    inline DeleteEventSourceMappingResult& AddQueues(const char* value) { m_queues.push_back(value); return *this; }
    ///@}

    ///@{
    /**
     * <p>An array of the authentication protocol, VPC components, or virtual host to
     * secure and define your event source.</p>
     */
    inline const Aws::Vector<SourceAccessConfiguration>& GetSourceAccessConfigurations() const{ return m_sourceAccessConfigurations; }
    inline void SetSourceAccessConfigurations(const Aws::Vector<SourceAccessConfiguration>& value) { m_sourceAccessConfigurations = value; }
    inline void SetSourceAccessConfigurations(Aws::Vector<SourceAccessConfiguration>&& value) { m_sourceAccessConfigurations = std::move(value); }
    inline DeleteEventSourceMappingResult& WithSourceAccessConfigurations(const Aws::Vector<SourceAccessConfiguration>& value) { SetSourceAccessConfigurations(value); return *this;}
    inline DeleteEventSourceMappingResult& WithSourceAccessConfigurations(Aws::Vector<SourceAccessConfiguration>&& value) { SetSourceAccessConfigurations(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& AddSourceAccessConfigurations(const SourceAccessConfiguration& value) { m_sourceAccessConfigurations.push_back(value); return *this; }
    inline DeleteEventSourceMappingResult& AddSourceAccessConfigurations(SourceAccessConfiguration&& value) { m_sourceAccessConfigurations.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The self-managed Apache Kafka cluster for your event source.</p>
     */
    inline const SelfManagedEventSource& GetSelfManagedEventSource() const{ return m_selfManagedEventSource; }
    inline void SetSelfManagedEventSource(const SelfManagedEventSource& value) { m_selfManagedEventSource = value; }
    inline void SetSelfManagedEventSource(SelfManagedEventSource&& value) { m_selfManagedEventSource = std::move(value); }
    inline DeleteEventSourceMappingResult& WithSelfManagedEventSource(const SelfManagedEventSource& value) { SetSelfManagedEventSource(value); return *this;}
    inline DeleteEventSourceMappingResult& WithSelfManagedEventSource(SelfManagedEventSource&& value) { SetSelfManagedEventSource(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>(Kinesis and DynamoDB Streams only) Discard records older than the specified
     * age. The default value is -1, which sets the maximum age to infinite. When the
     * value is set to infinite, Lambda never discards old records.</p>  <p>The
     * minimum valid value for maximum record age is 60s. Although values less than 60
     * and greater than -1 fall within the parameter's absolute range, they are not
     * allowed</p> 
     */
    inline int GetMaximumRecordAgeInSeconds() const{ return m_maximumRecordAgeInSeconds; }
    inline void SetMaximumRecordAgeInSeconds(int value) { m_maximumRecordAgeInSeconds = value; }
    inline DeleteEventSourceMappingResult& WithMaximumRecordAgeInSeconds(int value) { SetMaximumRecordAgeInSeconds(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>(Kinesis and DynamoDB Streams only) If the function returns an error, split
     * the batch in two and retry. The default value is false.</p>
     */
    inline bool GetBisectBatchOnFunctionError() const{ return m_bisectBatchOnFunctionError; }
    inline void SetBisectBatchOnFunctionError(bool value) { m_bisectBatchOnFunctionError = value; }
    inline DeleteEventSourceMappingResult& WithBisectBatchOnFunctionError(bool value) { SetBisectBatchOnFunctionError(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>(Kinesis and DynamoDB Streams only) Discard records after the specified
     * number of retries. The default value is -1, which sets the maximum number of
     * retries to infinite. When MaximumRetryAttempts is infinite, Lambda retries
     * failed records until the record expires in the event source.</p>
     */
    inline int GetMaximumRetryAttempts() const{ return m_maximumRetryAttempts; }
    inline void SetMaximumRetryAttempts(int value) { m_maximumRetryAttempts = value; }
    inline DeleteEventSourceMappingResult& WithMaximumRetryAttempts(int value) { SetMaximumRetryAttempts(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>(Kinesis and DynamoDB Streams only) The duration in seconds of a processing
     * window for DynamoDB and Kinesis Streams event sources. A value of 0 seconds
     * indicates no tumbling window.</p>
     */
    inline int GetTumblingWindowInSeconds() const{ return m_tumblingWindowInSeconds; }
    inline void SetTumblingWindowInSeconds(int value) { m_tumblingWindowInSeconds = value; }
    inline DeleteEventSourceMappingResult& WithTumblingWindowInSeconds(int value) { SetTumblingWindowInSeconds(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>(Kinesis, DynamoDB Streams, and Amazon SQS) A list of current response type
     * enums applied to the event source mapping.</p>
     */
    inline const Aws::Vector<FunctionResponseType>& GetFunctionResponseTypes() const{ return m_functionResponseTypes; }
    inline void SetFunctionResponseTypes(const Aws::Vector<FunctionResponseType>& value) { m_functionResponseTypes = value; }
    inline void SetFunctionResponseTypes(Aws::Vector<FunctionResponseType>&& value) { m_functionResponseTypes = std::move(value); }
    inline DeleteEventSourceMappingResult& WithFunctionResponseTypes(const Aws::Vector<FunctionResponseType>& value) { SetFunctionResponseTypes(value); return *this;}
    inline DeleteEventSourceMappingResult& WithFunctionResponseTypes(Aws::Vector<FunctionResponseType>&& value) { SetFunctionResponseTypes(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& AddFunctionResponseTypes(const FunctionResponseType& value) { m_functionResponseTypes.push_back(value); return *this; }
    inline DeleteEventSourceMappingResult& AddFunctionResponseTypes(FunctionResponseType&& value) { m_functionResponseTypes.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Specific configuration settings for an Amazon Managed Streaming for Apache
     * Kafka (Amazon MSK) event source.</p>
     */
    inline const AmazonManagedKafkaEventSourceConfig& GetAmazonManagedKafkaEventSourceConfig() const{ return m_amazonManagedKafkaEventSourceConfig; }
    inline void SetAmazonManagedKafkaEventSourceConfig(const AmazonManagedKafkaEventSourceConfig& value) { m_amazonManagedKafkaEventSourceConfig = value; }
    inline void SetAmazonManagedKafkaEventSourceConfig(AmazonManagedKafkaEventSourceConfig&& value) { m_amazonManagedKafkaEventSourceConfig = std::move(value); }
    inline DeleteEventSourceMappingResult& WithAmazonManagedKafkaEventSourceConfig(const AmazonManagedKafkaEventSourceConfig& value) { SetAmazonManagedKafkaEventSourceConfig(value); return *this;}
    inline DeleteEventSourceMappingResult& WithAmazonManagedKafkaEventSourceConfig(AmazonManagedKafkaEventSourceConfig&& value) { SetAmazonManagedKafkaEventSourceConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specific configuration settings for a self-managed Apache Kafka event
     * source.</p>
     */
    inline const SelfManagedKafkaEventSourceConfig& GetSelfManagedKafkaEventSourceConfig() const{ return m_selfManagedKafkaEventSourceConfig; }
    inline void SetSelfManagedKafkaEventSourceConfig(const SelfManagedKafkaEventSourceConfig& value) { m_selfManagedKafkaEventSourceConfig = value; }
    inline void SetSelfManagedKafkaEventSourceConfig(SelfManagedKafkaEventSourceConfig&& value) { m_selfManagedKafkaEventSourceConfig = std::move(value); }
    inline DeleteEventSourceMappingResult& WithSelfManagedKafkaEventSourceConfig(const SelfManagedKafkaEventSourceConfig& value) { SetSelfManagedKafkaEventSourceConfig(value); return *this;}
    inline DeleteEventSourceMappingResult& WithSelfManagedKafkaEventSourceConfig(SelfManagedKafkaEventSourceConfig&& value) { SetSelfManagedKafkaEventSourceConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>(Amazon SQS only) The scaling configuration for the event source. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/with-sqs.html#events-sqs-max-concurrency">Configuring
     * maximum concurrency for Amazon SQS event sources</a>.</p>
     */
    inline const ScalingConfig& GetScalingConfig() const{ return m_scalingConfig; }
    inline void SetScalingConfig(const ScalingConfig& value) { m_scalingConfig = value; }
    inline void SetScalingConfig(ScalingConfig&& value) { m_scalingConfig = std::move(value); }
    inline DeleteEventSourceMappingResult& WithScalingConfig(const ScalingConfig& value) { SetScalingConfig(value); return *this;}
    inline DeleteEventSourceMappingResult& WithScalingConfig(ScalingConfig&& value) { SetScalingConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specific configuration settings for a DocumentDB event source.</p>
     */
    inline const DocumentDBEventSourceConfig& GetDocumentDBEventSourceConfig() const{ return m_documentDBEventSourceConfig; }
    inline void SetDocumentDBEventSourceConfig(const DocumentDBEventSourceConfig& value) { m_documentDBEventSourceConfig = value; }
    inline void SetDocumentDBEventSourceConfig(DocumentDBEventSourceConfig&& value) { m_documentDBEventSourceConfig = std::move(value); }
    inline DeleteEventSourceMappingResult& WithDocumentDBEventSourceConfig(const DocumentDBEventSourceConfig& value) { SetDocumentDBEventSourceConfig(value); return *this;}
    inline DeleteEventSourceMappingResult& WithDocumentDBEventSourceConfig(DocumentDBEventSourceConfig&& value) { SetDocumentDBEventSourceConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p> The ARN of the Key Management Service (KMS) customer managed key that Lambda
     * uses to encrypt your function's <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-eventfiltering.html#filtering-basics">filter
     * criteria</a>.</p>
     */
    inline const Aws::String& GetKMSKeyArn() const{ return m_kMSKeyArn; }
    inline void SetKMSKeyArn(const Aws::String& value) { m_kMSKeyArn = value; }
    inline void SetKMSKeyArn(Aws::String&& value) { m_kMSKeyArn = std::move(value); }
    inline void SetKMSKeyArn(const char* value) { m_kMSKeyArn.assign(value); }
    inline DeleteEventSourceMappingResult& WithKMSKeyArn(const Aws::String& value) { SetKMSKeyArn(value); return *this;}
    inline DeleteEventSourceMappingResult& WithKMSKeyArn(Aws::String&& value) { SetKMSKeyArn(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& WithKMSKeyArn(const char* value) { SetKMSKeyArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An object that contains details about an error related to filter criteria
     * encryption.</p>
     */
    inline const FilterCriteriaError& GetFilterCriteriaError() const{ return m_filterCriteriaError; }
    inline void SetFilterCriteriaError(const FilterCriteriaError& value) { m_filterCriteriaError = value; }
    inline void SetFilterCriteriaError(FilterCriteriaError&& value) { m_filterCriteriaError = std::move(value); }
    inline DeleteEventSourceMappingResult& WithFilterCriteriaError(const FilterCriteriaError& value) { SetFilterCriteriaError(value); return *this;}
    inline DeleteEventSourceMappingResult& WithFilterCriteriaError(FilterCriteriaError&& value) { SetFilterCriteriaError(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the event source mapping.</p>
     */
    inline const Aws::String& GetEventSourceMappingArn() const{ return m_eventSourceMappingArn; }
    inline void SetEventSourceMappingArn(const Aws::String& value) { m_eventSourceMappingArn = value; }
    inline void SetEventSourceMappingArn(Aws::String&& value) { m_eventSourceMappingArn = std::move(value); }
    inline void SetEventSourceMappingArn(const char* value) { m_eventSourceMappingArn.assign(value); }
    inline DeleteEventSourceMappingResult& WithEventSourceMappingArn(const Aws::String& value) { SetEventSourceMappingArn(value); return *this;}
    inline DeleteEventSourceMappingResult& WithEventSourceMappingArn(Aws::String&& value) { SetEventSourceMappingArn(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& WithEventSourceMappingArn(const char* value) { SetEventSourceMappingArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The metrics configuration for your event source. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/monitoring-metrics-types.html#event-source-mapping-metrics">Event
     * source mapping metrics</a>.</p>
     */
    inline const EventSourceMappingMetricsConfig& GetMetricsConfig() const{ return m_metricsConfig; }
    inline void SetMetricsConfig(const EventSourceMappingMetricsConfig& value) { m_metricsConfig = value; }
    inline void SetMetricsConfig(EventSourceMappingMetricsConfig&& value) { m_metricsConfig = std::move(value); }
    inline DeleteEventSourceMappingResult& WithMetricsConfig(const EventSourceMappingMetricsConfig& value) { SetMetricsConfig(value); return *this;}
    inline DeleteEventSourceMappingResult& WithMetricsConfig(EventSourceMappingMetricsConfig&& value) { SetMetricsConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>(Amazon MSK and self-managed Apache Kafka only) The Provisioned Mode
     * configuration for the event source. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-eventsourcemapping.html#invocation-eventsourcemapping-provisioned-mode">Provisioned
     * Mode</a>.</p>
     */
    inline const ProvisionedPollerConfig& GetProvisionedPollerConfig() const{ return m_provisionedPollerConfig; }
    inline void SetProvisionedPollerConfig(const ProvisionedPollerConfig& value) { m_provisionedPollerConfig = value; }
    inline void SetProvisionedPollerConfig(ProvisionedPollerConfig&& value) { m_provisionedPollerConfig = std::move(value); }
    inline DeleteEventSourceMappingResult& WithProvisionedPollerConfig(const ProvisionedPollerConfig& value) { SetProvisionedPollerConfig(value); return *this;}
    inline DeleteEventSourceMappingResult& WithProvisionedPollerConfig(ProvisionedPollerConfig&& value) { SetProvisionedPollerConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline DeleteEventSourceMappingResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline DeleteEventSourceMappingResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline DeleteEventSourceMappingResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_uUID;

    EventSourcePosition m_startingPosition;

    Aws::Utils::DateTime m_startingPositionTimestamp;

    int m_batchSize;

    int m_maximumBatchingWindowInSeconds;

    int m_parallelizationFactor;

    Aws::String m_eventSourceArn;

    FilterCriteria m_filterCriteria;

    Aws::String m_functionArn;

    Aws::Utils::DateTime m_lastModified;

    Aws::String m_lastProcessingResult;

    Aws::String m_state;

    Aws::String m_stateTransitionReason;

    DestinationConfig m_destinationConfig;

    Aws::Vector<Aws::String> m_topics;

    Aws::Vector<Aws::String> m_queues;

    Aws::Vector<SourceAccessConfiguration> m_sourceAccessConfigurations;

    SelfManagedEventSource m_selfManagedEventSource;

    int m_maximumRecordAgeInSeconds;

    bool m_bisectBatchOnFunctionError;

    int m_maximumRetryAttempts;

    int m_tumblingWindowInSeconds;

    Aws::Vector<FunctionResponseType> m_functionResponseTypes;

    AmazonManagedKafkaEventSourceConfig m_amazonManagedKafkaEventSourceConfig;

    SelfManagedKafkaEventSourceConfig m_selfManagedKafkaEventSourceConfig;

    ScalingConfig m_scalingConfig;

    DocumentDBEventSourceConfig m_documentDBEventSourceConfig;

    Aws::String m_kMSKeyArn;

    FilterCriteriaError m_filterCriteriaError;

    Aws::String m_eventSourceMappingArn;

    EventSourceMappingMetricsConfig m_metricsConfig;

    ProvisionedPollerConfig m_provisionedPollerConfig;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
