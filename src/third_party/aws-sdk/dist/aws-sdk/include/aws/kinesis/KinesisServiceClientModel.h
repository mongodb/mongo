/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

/* Generic header includes */
#include <aws/kinesis/KinesisErrors.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/client/AsyncCallerContext.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/kinesis/KinesisEndpointProvider.h>
#include <future>
#include <functional>
/* End of generic header includes */

/* Service model headers required in KinesisClient header */
#include <aws/kinesis/model/DescribeLimitsResult.h>
#include <aws/kinesis/model/DescribeStreamResult.h>
#include <aws/kinesis/model/DescribeStreamConsumerResult.h>
#include <aws/kinesis/model/DescribeStreamSummaryResult.h>
#include <aws/kinesis/model/DisableEnhancedMonitoringResult.h>
#include <aws/kinesis/model/EnableEnhancedMonitoringResult.h>
#include <aws/kinesis/model/GetRecordsResult.h>
#include <aws/kinesis/model/GetResourcePolicyResult.h>
#include <aws/kinesis/model/GetShardIteratorResult.h>
#include <aws/kinesis/model/ListShardsResult.h>
#include <aws/kinesis/model/ListStreamConsumersResult.h>
#include <aws/kinesis/model/ListStreamsResult.h>
#include <aws/kinesis/model/ListTagsForStreamResult.h>
#include <aws/kinesis/model/PutRecordResult.h>
#include <aws/kinesis/model/PutRecordsResult.h>
#include <aws/kinesis/model/RegisterStreamConsumerResult.h>
#include <aws/kinesis/model/UpdateShardCountResult.h>
#include <aws/kinesis/model/DescribeLimitsRequest.h>
#include <aws/kinesis/model/DeregisterStreamConsumerRequest.h>
#include <aws/kinesis/model/ListShardsRequest.h>
#include <aws/kinesis/model/ListStreamsRequest.h>
#include <aws/kinesis/model/DescribeStreamSummaryRequest.h>
#include <aws/kinesis/model/DeleteStreamRequest.h>
#include <aws/kinesis/model/DescribeStreamConsumerRequest.h>
#include <aws/kinesis/model/ListTagsForStreamRequest.h>
#include <aws/kinesis/model/DescribeStreamRequest.h>
#include <aws/core/NoResult.h>
/* End of service model headers required in KinesisClient header */

namespace Aws
{
  namespace Http
  {
    class HttpClient;
    class HttpClientFactory;
  } // namespace Http

  namespace Utils
  {
    template< typename R, typename E> class Outcome;

    namespace Threading
    {
      class Executor;
    } // namespace Threading
  } // namespace Utils

  namespace Auth
  {
    class AWSCredentials;
    class AWSCredentialsProvider;
  } // namespace Auth

  namespace Client
  {
    class RetryStrategy;
  } // namespace Client

  namespace Kinesis
  {
    using KinesisClientConfiguration = Aws::Client::GenericClientConfiguration;
    using KinesisEndpointProviderBase = Aws::Kinesis::Endpoint::KinesisEndpointProviderBase;
    using KinesisEndpointProvider = Aws::Kinesis::Endpoint::KinesisEndpointProvider;

    namespace Model
    {
      /* Service model forward declarations required in KinesisClient header */
      class AddTagsToStreamRequest;
      class CreateStreamRequest;
      class DecreaseStreamRetentionPeriodRequest;
      class DeleteResourcePolicyRequest;
      class DeleteStreamRequest;
      class DeregisterStreamConsumerRequest;
      class DescribeLimitsRequest;
      class DescribeStreamRequest;
      class DescribeStreamConsumerRequest;
      class DescribeStreamSummaryRequest;
      class DisableEnhancedMonitoringRequest;
      class EnableEnhancedMonitoringRequest;
      class GetRecordsRequest;
      class GetResourcePolicyRequest;
      class GetShardIteratorRequest;
      class IncreaseStreamRetentionPeriodRequest;
      class ListShardsRequest;
      class ListStreamConsumersRequest;
      class ListStreamsRequest;
      class ListTagsForStreamRequest;
      class MergeShardsRequest;
      class PutRecordRequest;
      class PutRecordsRequest;
      class PutResourcePolicyRequest;
      class RegisterStreamConsumerRequest;
      class RemoveTagsFromStreamRequest;
      class SplitShardRequest;
      class StartStreamEncryptionRequest;
      class StopStreamEncryptionRequest;
      class SubscribeToShardRequest;
      class UpdateShardCountRequest;
      class UpdateStreamModeRequest;
      /* End of service model forward declarations required in KinesisClient header */

      /* Service model Outcome class definitions */
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> AddTagsToStreamOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> CreateStreamOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> DecreaseStreamRetentionPeriodOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> DeleteResourcePolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> DeleteStreamOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> DeregisterStreamConsumerOutcome;
      typedef Aws::Utils::Outcome<DescribeLimitsResult, KinesisError> DescribeLimitsOutcome;
      typedef Aws::Utils::Outcome<DescribeStreamResult, KinesisError> DescribeStreamOutcome;
      typedef Aws::Utils::Outcome<DescribeStreamConsumerResult, KinesisError> DescribeStreamConsumerOutcome;
      typedef Aws::Utils::Outcome<DescribeStreamSummaryResult, KinesisError> DescribeStreamSummaryOutcome;
      typedef Aws::Utils::Outcome<DisableEnhancedMonitoringResult, KinesisError> DisableEnhancedMonitoringOutcome;
      typedef Aws::Utils::Outcome<EnableEnhancedMonitoringResult, KinesisError> EnableEnhancedMonitoringOutcome;
      typedef Aws::Utils::Outcome<GetRecordsResult, KinesisError> GetRecordsOutcome;
      typedef Aws::Utils::Outcome<GetResourcePolicyResult, KinesisError> GetResourcePolicyOutcome;
      typedef Aws::Utils::Outcome<GetShardIteratorResult, KinesisError> GetShardIteratorOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> IncreaseStreamRetentionPeriodOutcome;
      typedef Aws::Utils::Outcome<ListShardsResult, KinesisError> ListShardsOutcome;
      typedef Aws::Utils::Outcome<ListStreamConsumersResult, KinesisError> ListStreamConsumersOutcome;
      typedef Aws::Utils::Outcome<ListStreamsResult, KinesisError> ListStreamsOutcome;
      typedef Aws::Utils::Outcome<ListTagsForStreamResult, KinesisError> ListTagsForStreamOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> MergeShardsOutcome;
      typedef Aws::Utils::Outcome<PutRecordResult, KinesisError> PutRecordOutcome;
      typedef Aws::Utils::Outcome<PutRecordsResult, KinesisError> PutRecordsOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> PutResourcePolicyOutcome;
      typedef Aws::Utils::Outcome<RegisterStreamConsumerResult, KinesisError> RegisterStreamConsumerOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> RemoveTagsFromStreamOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> SplitShardOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> StartStreamEncryptionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> StopStreamEncryptionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> SubscribeToShardOutcome;
      typedef Aws::Utils::Outcome<UpdateShardCountResult, KinesisError> UpdateShardCountOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, KinesisError> UpdateStreamModeOutcome;
      /* End of service model Outcome class definitions */

      /* Service model Outcome callable definitions */
      typedef std::future<AddTagsToStreamOutcome> AddTagsToStreamOutcomeCallable;
      typedef std::future<CreateStreamOutcome> CreateStreamOutcomeCallable;
      typedef std::future<DecreaseStreamRetentionPeriodOutcome> DecreaseStreamRetentionPeriodOutcomeCallable;
      typedef std::future<DeleteResourcePolicyOutcome> DeleteResourcePolicyOutcomeCallable;
      typedef std::future<DeleteStreamOutcome> DeleteStreamOutcomeCallable;
      typedef std::future<DeregisterStreamConsumerOutcome> DeregisterStreamConsumerOutcomeCallable;
      typedef std::future<DescribeLimitsOutcome> DescribeLimitsOutcomeCallable;
      typedef std::future<DescribeStreamOutcome> DescribeStreamOutcomeCallable;
      typedef std::future<DescribeStreamConsumerOutcome> DescribeStreamConsumerOutcomeCallable;
      typedef std::future<DescribeStreamSummaryOutcome> DescribeStreamSummaryOutcomeCallable;
      typedef std::future<DisableEnhancedMonitoringOutcome> DisableEnhancedMonitoringOutcomeCallable;
      typedef std::future<EnableEnhancedMonitoringOutcome> EnableEnhancedMonitoringOutcomeCallable;
      typedef std::future<GetRecordsOutcome> GetRecordsOutcomeCallable;
      typedef std::future<GetResourcePolicyOutcome> GetResourcePolicyOutcomeCallable;
      typedef std::future<GetShardIteratorOutcome> GetShardIteratorOutcomeCallable;
      typedef std::future<IncreaseStreamRetentionPeriodOutcome> IncreaseStreamRetentionPeriodOutcomeCallable;
      typedef std::future<ListShardsOutcome> ListShardsOutcomeCallable;
      typedef std::future<ListStreamConsumersOutcome> ListStreamConsumersOutcomeCallable;
      typedef std::future<ListStreamsOutcome> ListStreamsOutcomeCallable;
      typedef std::future<ListTagsForStreamOutcome> ListTagsForStreamOutcomeCallable;
      typedef std::future<MergeShardsOutcome> MergeShardsOutcomeCallable;
      typedef std::future<PutRecordOutcome> PutRecordOutcomeCallable;
      typedef std::future<PutRecordsOutcome> PutRecordsOutcomeCallable;
      typedef std::future<PutResourcePolicyOutcome> PutResourcePolicyOutcomeCallable;
      typedef std::future<RegisterStreamConsumerOutcome> RegisterStreamConsumerOutcomeCallable;
      typedef std::future<RemoveTagsFromStreamOutcome> RemoveTagsFromStreamOutcomeCallable;
      typedef std::future<SplitShardOutcome> SplitShardOutcomeCallable;
      typedef std::future<StartStreamEncryptionOutcome> StartStreamEncryptionOutcomeCallable;
      typedef std::future<StopStreamEncryptionOutcome> StopStreamEncryptionOutcomeCallable;
      typedef std::future<SubscribeToShardOutcome> SubscribeToShardOutcomeCallable;
      typedef std::future<UpdateShardCountOutcome> UpdateShardCountOutcomeCallable;
      typedef std::future<UpdateStreamModeOutcome> UpdateStreamModeOutcomeCallable;
      /* End of service model Outcome callable definitions */
    } // namespace Model

    class KinesisClient;

    /* Service model async handlers definitions */
    typedef std::function<void(const KinesisClient*, const Model::AddTagsToStreamRequest&, const Model::AddTagsToStreamOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > AddTagsToStreamResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::CreateStreamRequest&, const Model::CreateStreamOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateStreamResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::DecreaseStreamRetentionPeriodRequest&, const Model::DecreaseStreamRetentionPeriodOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DecreaseStreamRetentionPeriodResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::DeleteResourcePolicyRequest&, const Model::DeleteResourcePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteResourcePolicyResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::DeleteStreamRequest&, const Model::DeleteStreamOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteStreamResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::DeregisterStreamConsumerRequest&, const Model::DeregisterStreamConsumerOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeregisterStreamConsumerResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::DescribeLimitsRequest&, const Model::DescribeLimitsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DescribeLimitsResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::DescribeStreamRequest&, const Model::DescribeStreamOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DescribeStreamResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::DescribeStreamConsumerRequest&, const Model::DescribeStreamConsumerOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DescribeStreamConsumerResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::DescribeStreamSummaryRequest&, const Model::DescribeStreamSummaryOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DescribeStreamSummaryResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::DisableEnhancedMonitoringRequest&, const Model::DisableEnhancedMonitoringOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DisableEnhancedMonitoringResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::EnableEnhancedMonitoringRequest&, const Model::EnableEnhancedMonitoringOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > EnableEnhancedMonitoringResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::GetRecordsRequest&, const Model::GetRecordsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetRecordsResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::GetResourcePolicyRequest&, const Model::GetResourcePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetResourcePolicyResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::GetShardIteratorRequest&, const Model::GetShardIteratorOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetShardIteratorResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::IncreaseStreamRetentionPeriodRequest&, const Model::IncreaseStreamRetentionPeriodOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > IncreaseStreamRetentionPeriodResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::ListShardsRequest&, const Model::ListShardsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListShardsResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::ListStreamConsumersRequest&, const Model::ListStreamConsumersOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListStreamConsumersResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::ListStreamsRequest&, const Model::ListStreamsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListStreamsResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::ListTagsForStreamRequest&, const Model::ListTagsForStreamOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListTagsForStreamResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::MergeShardsRequest&, const Model::MergeShardsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > MergeShardsResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::PutRecordRequest&, const Model::PutRecordOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutRecordResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::PutRecordsRequest&, const Model::PutRecordsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutRecordsResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::PutResourcePolicyRequest&, const Model::PutResourcePolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutResourcePolicyResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::RegisterStreamConsumerRequest&, const Model::RegisterStreamConsumerOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > RegisterStreamConsumerResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::RemoveTagsFromStreamRequest&, const Model::RemoveTagsFromStreamOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > RemoveTagsFromStreamResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::SplitShardRequest&, const Model::SplitShardOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > SplitShardResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::StartStreamEncryptionRequest&, const Model::StartStreamEncryptionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > StartStreamEncryptionResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::StopStreamEncryptionRequest&, const Model::StopStreamEncryptionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > StopStreamEncryptionResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::SubscribeToShardRequest&, const Model::SubscribeToShardOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > SubscribeToShardResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::UpdateShardCountRequest&, const Model::UpdateShardCountOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateShardCountResponseReceivedHandler;
    typedef std::function<void(const KinesisClient*, const Model::UpdateStreamModeRequest&, const Model::UpdateStreamModeOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UpdateStreamModeResponseReceivedHandler;
    /* End of service model async handlers definitions */
  } // namespace Kinesis
} // namespace Aws
