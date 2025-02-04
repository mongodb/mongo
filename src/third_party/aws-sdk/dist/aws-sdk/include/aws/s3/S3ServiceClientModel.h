/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

/* Generic header includes */
#include <aws/s3/S3Errors.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/client/AsyncCallerContext.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/s3/S3EndpointProvider.h>
#include <future>
#include <functional>
/* End of generic header includes */

/* Service model headers required in S3Client header */
#include <aws/s3/model/AbortMultipartUploadResult.h>
#include <aws/s3/model/CompleteMultipartUploadResult.h>
#include <aws/s3/model/CopyObjectResult.h>
#include <aws/s3/model/CreateBucketResult.h>
#include <aws/s3/model/CreateMultipartUploadResult.h>
#include <aws/s3/model/CreateSessionResult.h>
#include <aws/s3/model/DeleteObjectResult.h>
#include <aws/s3/model/DeleteObjectTaggingResult.h>
#include <aws/s3/model/DeleteObjectsResult.h>
#include <aws/s3/model/GetBucketAccelerateConfigurationResult.h>
#include <aws/s3/model/GetBucketAclResult.h>
#include <aws/s3/model/GetBucketAnalyticsConfigurationResult.h>
#include <aws/s3/model/GetBucketCorsResult.h>
#include <aws/s3/model/GetBucketEncryptionResult.h>
#include <aws/s3/model/GetBucketIntelligentTieringConfigurationResult.h>
#include <aws/s3/model/GetBucketInventoryConfigurationResult.h>
#include <aws/s3/model/GetBucketLifecycleConfigurationResult.h>
#include <aws/s3/model/GetBucketLocationResult.h>
#include <aws/s3/model/GetBucketLoggingResult.h>
#include <aws/s3/model/GetBucketMetadataTableConfigurationSdkResult.h>
#include <aws/s3/model/GetBucketMetricsConfigurationResult.h>
#include <aws/s3/model/GetBucketNotificationConfigurationResult.h>
#include <aws/s3/model/GetBucketOwnershipControlsResult.h>
#include <aws/s3/model/GetBucketPolicyResult.h>
#include <aws/s3/model/GetBucketPolicyStatusResult.h>
#include <aws/s3/model/GetBucketReplicationResult.h>
#include <aws/s3/model/GetBucketRequestPaymentResult.h>
#include <aws/s3/model/GetBucketTaggingResult.h>
#include <aws/s3/model/GetBucketVersioningResult.h>
#include <aws/s3/model/GetBucketWebsiteResult.h>
#include <aws/s3/model/GetObjectResult.h>
#include <aws/s3/model/GetObjectAclResult.h>
#include <aws/s3/model/GetObjectAttributesResult.h>
#include <aws/s3/model/GetObjectLegalHoldResult.h>
#include <aws/s3/model/GetObjectLockConfigurationResult.h>
#include <aws/s3/model/GetObjectRetentionResult.h>
#include <aws/s3/model/GetObjectTaggingResult.h>
#include <aws/s3/model/GetObjectTorrentResult.h>
#include <aws/s3/model/GetPublicAccessBlockResult.h>
#include <aws/s3/model/HeadBucketResult.h>
#include <aws/s3/model/HeadObjectResult.h>
#include <aws/s3/model/ListBucketAnalyticsConfigurationsResult.h>
#include <aws/s3/model/ListBucketIntelligentTieringConfigurationsResult.h>
#include <aws/s3/model/ListBucketInventoryConfigurationsResult.h>
#include <aws/s3/model/ListBucketMetricsConfigurationsResult.h>
#include <aws/s3/model/ListBucketsResult.h>
#include <aws/s3/model/ListDirectoryBucketsResult.h>
#include <aws/s3/model/ListMultipartUploadsResult.h>
#include <aws/s3/model/ListObjectVersionsResult.h>
#include <aws/s3/model/ListObjectsResult.h>
#include <aws/s3/model/ListObjectsV2Result.h>
#include <aws/s3/model/ListPartsResult.h>
#include <aws/s3/model/PutBucketLifecycleConfigurationResult.h>
#include <aws/s3/model/PutObjectResult.h>
#include <aws/s3/model/PutObjectAclResult.h>
#include <aws/s3/model/PutObjectLegalHoldResult.h>
#include <aws/s3/model/PutObjectLockConfigurationResult.h>
#include <aws/s3/model/PutObjectRetentionResult.h>
#include <aws/s3/model/PutObjectTaggingResult.h>
#include <aws/s3/model/RestoreObjectResult.h>
#include <aws/s3/model/UploadPartResult.h>
#include <aws/s3/model/UploadPartCopyResult.h>
#include <aws/s3/model/ListDirectoryBucketsRequest.h>
#include <aws/s3/model/ListBucketsRequest.h>
#include <aws/core/NoResult.h>
/* End of service model headers required in S3Client header */

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

  namespace S3
  {
    using S3EndpointProviderBase = Aws::S3::Endpoint::S3EndpointProviderBase;
    using S3EndpointProvider = Aws::S3::Endpoint::S3EndpointProvider;

    namespace Model
    {
      /* Service model forward declarations required in S3Client header */
      class AbortMultipartUploadRequest;
      class CompleteMultipartUploadRequest;
      class CopyObjectRequest;
      class CreateBucketRequest;
      class CreateBucketMetadataTableConfigurationRequest;
      class CreateMultipartUploadRequest;
      class CreateSessionRequest;
      class DeleteBucketRequest;
      class DeleteBucketAnalyticsConfigurationRequest;
      class DeleteBucketCorsRequest;
      class DeleteBucketEncryptionRequest;
      class DeleteBucketIntelligentTieringConfigurationRequest;
      class DeleteBucketInventoryConfigurationRequest;
      class DeleteBucketLifecycleRequest;
      class DeleteBucketMetadataTableConfigurationRequest;
      class DeleteBucketMetricsConfigurationRequest;
      class DeleteBucketOwnershipControlsRequest;
      class DeleteBucketPolicyRequest;
      class DeleteBucketReplicationRequest;
      class DeleteBucketTaggingRequest;
      class DeleteBucketWebsiteRequest;
      class DeleteObjectRequest;
      class DeleteObjectTaggingRequest;
      class DeleteObjectsRequest;
      class DeletePublicAccessBlockRequest;
      class GetBucketAccelerateConfigurationRequest;
      class GetBucketAclRequest;
      class GetBucketAnalyticsConfigurationRequest;
      class GetBucketCorsRequest;
      class GetBucketEncryptionRequest;
      class GetBucketIntelligentTieringConfigurationRequest;
      class GetBucketInventoryConfigurationRequest;
      class GetBucketLifecycleConfigurationRequest;
      class GetBucketLocationRequest;
      class GetBucketLoggingRequest;
      class GetBucketMetadataTableConfigurationRequest;
      class GetBucketMetricsConfigurationRequest;
      class GetBucketNotificationConfigurationRequest;
      class GetBucketOwnershipControlsRequest;
      class GetBucketPolicyRequest;
      class GetBucketPolicyStatusRequest;
      class GetBucketReplicationRequest;
      class GetBucketRequestPaymentRequest;
      class GetBucketTaggingRequest;
      class GetBucketVersioningRequest;
      class GetBucketWebsiteRequest;
      class GetObjectRequest;
      class GetObjectAclRequest;
      class GetObjectAttributesRequest;
      class GetObjectLegalHoldRequest;
      class GetObjectLockConfigurationRequest;
      class GetObjectRetentionRequest;
      class GetObjectTaggingRequest;
      class GetObjectTorrentRequest;
      class GetPublicAccessBlockRequest;
      class HeadBucketRequest;
      class HeadObjectRequest;
      class ListBucketAnalyticsConfigurationsRequest;
      class ListBucketIntelligentTieringConfigurationsRequest;
      class ListBucketInventoryConfigurationsRequest;
      class ListBucketMetricsConfigurationsRequest;
      class ListBucketsRequest;
      class ListDirectoryBucketsRequest;
      class ListMultipartUploadsRequest;
      class ListObjectVersionsRequest;
      class ListObjectsRequest;
      class ListObjectsV2Request;
      class ListPartsRequest;
      class PutBucketAccelerateConfigurationRequest;
      class PutBucketAclRequest;
      class PutBucketAnalyticsConfigurationRequest;
      class PutBucketCorsRequest;
      class PutBucketEncryptionRequest;
      class PutBucketIntelligentTieringConfigurationRequest;
      class PutBucketInventoryConfigurationRequest;
      class PutBucketLifecycleConfigurationRequest;
      class PutBucketLoggingRequest;
      class PutBucketMetricsConfigurationRequest;
      class PutBucketNotificationConfigurationRequest;
      class PutBucketOwnershipControlsRequest;
      class PutBucketPolicyRequest;
      class PutBucketReplicationRequest;
      class PutBucketRequestPaymentRequest;
      class PutBucketTaggingRequest;
      class PutBucketVersioningRequest;
      class PutBucketWebsiteRequest;
      class PutObjectRequest;
      class PutObjectAclRequest;
      class PutObjectLegalHoldRequest;
      class PutObjectLockConfigurationRequest;
      class PutObjectRetentionRequest;
      class PutObjectTaggingRequest;
      class PutPublicAccessBlockRequest;
      class RestoreObjectRequest;
      class SelectObjectContentRequest;
      class UploadPartRequest;
      class UploadPartCopyRequest;
      class WriteGetObjectResponseRequest;
      /* End of service model forward declarations required in S3Client header */

      /* Service model Outcome class definitions */
      typedef Aws::Utils::Outcome<AbortMultipartUploadResult, S3Error> AbortMultipartUploadOutcome;
      typedef Aws::Utils::Outcome<CompleteMultipartUploadResult, S3Error> CompleteMultipartUploadOutcome;
      typedef Aws::Utils::Outcome<CopyObjectResult, S3Error> CopyObjectOutcome;
      typedef Aws::Utils::Outcome<CreateBucketResult, S3Error> CreateBucketOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> CreateBucketMetadataTableConfigurationOutcome;
      typedef Aws::Utils::Outcome<CreateMultipartUploadResult, S3Error> CreateMultipartUploadOutcome;
      typedef Aws::Utils::Outcome<CreateSessionResult, S3Error> CreateSessionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketAnalyticsConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketCorsOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketEncryptionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketIntelligentTieringConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketInventoryConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketLifecycleOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketMetadataTableConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketMetricsConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketOwnershipControlsOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketReplicationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketTaggingOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeleteBucketWebsiteOutcome;
      typedef Aws::Utils::Outcome<DeleteObjectResult, S3Error> DeleteObjectOutcome;
      typedef Aws::Utils::Outcome<DeleteObjectTaggingResult, S3Error> DeleteObjectTaggingOutcome;
      typedef Aws::Utils::Outcome<DeleteObjectsResult, S3Error> DeleteObjectsOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> DeletePublicAccessBlockOutcome;
      typedef Aws::Utils::Outcome<GetBucketAccelerateConfigurationResult, S3Error> GetBucketAccelerateConfigurationOutcome;
      typedef Aws::Utils::Outcome<GetBucketAclResult, S3Error> GetBucketAclOutcome;
      typedef Aws::Utils::Outcome<GetBucketAnalyticsConfigurationResult, S3Error> GetBucketAnalyticsConfigurationOutcome;
      typedef Aws::Utils::Outcome<GetBucketCorsResult, S3Error> GetBucketCorsOutcome;
      typedef Aws::Utils::Outcome<GetBucketEncryptionResult, S3Error> GetBucketEncryptionOutcome;
      typedef Aws::Utils::Outcome<GetBucketIntelligentTieringConfigurationResult, S3Error> GetBucketIntelligentTieringConfigurationOutcome;
      typedef Aws::Utils::Outcome<GetBucketInventoryConfigurationResult, S3Error> GetBucketInventoryConfigurationOutcome;
      typedef Aws::Utils::Outcome<GetBucketLifecycleConfigurationResult, S3Error> GetBucketLifecycleConfigurationOutcome;
      typedef Aws::Utils::Outcome<GetBucketLocationResult, S3Error> GetBucketLocationOutcome;
      typedef Aws::Utils::Outcome<GetBucketLoggingResult, S3Error> GetBucketLoggingOutcome;
      typedef Aws::Utils::Outcome<GetBucketMetadataTableConfigurationSdkResult, S3Error> GetBucketMetadataTableConfigurationOutcome;
      typedef Aws::Utils::Outcome<GetBucketMetricsConfigurationResult, S3Error> GetBucketMetricsConfigurationOutcome;
      typedef Aws::Utils::Outcome<GetBucketNotificationConfigurationResult, S3Error> GetBucketNotificationConfigurationOutcome;
      typedef Aws::Utils::Outcome<GetBucketOwnershipControlsResult, S3Error> GetBucketOwnershipControlsOutcome;
      typedef Aws::Utils::Outcome<GetBucketPolicyResult, S3Error> GetBucketPolicyOutcome;
      typedef Aws::Utils::Outcome<GetBucketPolicyStatusResult, S3Error> GetBucketPolicyStatusOutcome;
      typedef Aws::Utils::Outcome<GetBucketReplicationResult, S3Error> GetBucketReplicationOutcome;
      typedef Aws::Utils::Outcome<GetBucketRequestPaymentResult, S3Error> GetBucketRequestPaymentOutcome;
      typedef Aws::Utils::Outcome<GetBucketTaggingResult, S3Error> GetBucketTaggingOutcome;
      typedef Aws::Utils::Outcome<GetBucketVersioningResult, S3Error> GetBucketVersioningOutcome;
      typedef Aws::Utils::Outcome<GetBucketWebsiteResult, S3Error> GetBucketWebsiteOutcome;
      typedef Aws::Utils::Outcome<GetObjectResult, S3Error> GetObjectOutcome;
      typedef Aws::Utils::Outcome<GetObjectAclResult, S3Error> GetObjectAclOutcome;
      typedef Aws::Utils::Outcome<GetObjectAttributesResult, S3Error> GetObjectAttributesOutcome;
      typedef Aws::Utils::Outcome<GetObjectLegalHoldResult, S3Error> GetObjectLegalHoldOutcome;
      typedef Aws::Utils::Outcome<GetObjectLockConfigurationResult, S3Error> GetObjectLockConfigurationOutcome;
      typedef Aws::Utils::Outcome<GetObjectRetentionResult, S3Error> GetObjectRetentionOutcome;
      typedef Aws::Utils::Outcome<GetObjectTaggingResult, S3Error> GetObjectTaggingOutcome;
      typedef Aws::Utils::Outcome<GetObjectTorrentResult, S3Error> GetObjectTorrentOutcome;
      typedef Aws::Utils::Outcome<GetPublicAccessBlockResult, S3Error> GetPublicAccessBlockOutcome;
      typedef Aws::Utils::Outcome<HeadBucketResult, S3Error> HeadBucketOutcome;
      typedef Aws::Utils::Outcome<HeadObjectResult, S3Error> HeadObjectOutcome;
      typedef Aws::Utils::Outcome<ListBucketAnalyticsConfigurationsResult, S3Error> ListBucketAnalyticsConfigurationsOutcome;
      typedef Aws::Utils::Outcome<ListBucketIntelligentTieringConfigurationsResult, S3Error> ListBucketIntelligentTieringConfigurationsOutcome;
      typedef Aws::Utils::Outcome<ListBucketInventoryConfigurationsResult, S3Error> ListBucketInventoryConfigurationsOutcome;
      typedef Aws::Utils::Outcome<ListBucketMetricsConfigurationsResult, S3Error> ListBucketMetricsConfigurationsOutcome;
      typedef Aws::Utils::Outcome<ListBucketsResult, S3Error> ListBucketsOutcome;
      typedef Aws::Utils::Outcome<ListDirectoryBucketsResult, S3Error> ListDirectoryBucketsOutcome;
      typedef Aws::Utils::Outcome<ListMultipartUploadsResult, S3Error> ListMultipartUploadsOutcome;
      typedef Aws::Utils::Outcome<ListObjectVersionsResult, S3Error> ListObjectVersionsOutcome;
      typedef Aws::Utils::Outcome<ListObjectsResult, S3Error> ListObjectsOutcome;
      typedef Aws::Utils::Outcome<ListObjectsV2Result, S3Error> ListObjectsV2Outcome;
      typedef Aws::Utils::Outcome<ListPartsResult, S3Error> ListPartsOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketAccelerateConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketAclOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketAnalyticsConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketCorsOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketEncryptionOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketIntelligentTieringConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketInventoryConfigurationOutcome;
      typedef Aws::Utils::Outcome<PutBucketLifecycleConfigurationResult, S3Error> PutBucketLifecycleConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketLoggingOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketMetricsConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketNotificationConfigurationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketOwnershipControlsOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketPolicyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketReplicationOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketRequestPaymentOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketTaggingOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketVersioningOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutBucketWebsiteOutcome;
      typedef Aws::Utils::Outcome<PutObjectResult, S3Error> PutObjectOutcome;
      typedef Aws::Utils::Outcome<PutObjectAclResult, S3Error> PutObjectAclOutcome;
      typedef Aws::Utils::Outcome<PutObjectLegalHoldResult, S3Error> PutObjectLegalHoldOutcome;
      typedef Aws::Utils::Outcome<PutObjectLockConfigurationResult, S3Error> PutObjectLockConfigurationOutcome;
      typedef Aws::Utils::Outcome<PutObjectRetentionResult, S3Error> PutObjectRetentionOutcome;
      typedef Aws::Utils::Outcome<PutObjectTaggingResult, S3Error> PutObjectTaggingOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> PutPublicAccessBlockOutcome;
      typedef Aws::Utils::Outcome<RestoreObjectResult, S3Error> RestoreObjectOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> SelectObjectContentOutcome;
      typedef Aws::Utils::Outcome<UploadPartResult, S3Error> UploadPartOutcome;
      typedef Aws::Utils::Outcome<UploadPartCopyResult, S3Error> UploadPartCopyOutcome;
      typedef Aws::Utils::Outcome<Aws::NoResult, S3Error> WriteGetObjectResponseOutcome;
      /* End of service model Outcome class definitions */

      /* Service model Outcome callable definitions */
      typedef std::future<AbortMultipartUploadOutcome> AbortMultipartUploadOutcomeCallable;
      typedef std::future<CompleteMultipartUploadOutcome> CompleteMultipartUploadOutcomeCallable;
      typedef std::future<CopyObjectOutcome> CopyObjectOutcomeCallable;
      typedef std::future<CreateBucketOutcome> CreateBucketOutcomeCallable;
      typedef std::future<CreateBucketMetadataTableConfigurationOutcome> CreateBucketMetadataTableConfigurationOutcomeCallable;
      typedef std::future<CreateMultipartUploadOutcome> CreateMultipartUploadOutcomeCallable;
      typedef std::future<CreateSessionOutcome> CreateSessionOutcomeCallable;
      typedef std::future<DeleteBucketOutcome> DeleteBucketOutcomeCallable;
      typedef std::future<DeleteBucketAnalyticsConfigurationOutcome> DeleteBucketAnalyticsConfigurationOutcomeCallable;
      typedef std::future<DeleteBucketCorsOutcome> DeleteBucketCorsOutcomeCallable;
      typedef std::future<DeleteBucketEncryptionOutcome> DeleteBucketEncryptionOutcomeCallable;
      typedef std::future<DeleteBucketIntelligentTieringConfigurationOutcome> DeleteBucketIntelligentTieringConfigurationOutcomeCallable;
      typedef std::future<DeleteBucketInventoryConfigurationOutcome> DeleteBucketInventoryConfigurationOutcomeCallable;
      typedef std::future<DeleteBucketLifecycleOutcome> DeleteBucketLifecycleOutcomeCallable;
      typedef std::future<DeleteBucketMetadataTableConfigurationOutcome> DeleteBucketMetadataTableConfigurationOutcomeCallable;
      typedef std::future<DeleteBucketMetricsConfigurationOutcome> DeleteBucketMetricsConfigurationOutcomeCallable;
      typedef std::future<DeleteBucketOwnershipControlsOutcome> DeleteBucketOwnershipControlsOutcomeCallable;
      typedef std::future<DeleteBucketPolicyOutcome> DeleteBucketPolicyOutcomeCallable;
      typedef std::future<DeleteBucketReplicationOutcome> DeleteBucketReplicationOutcomeCallable;
      typedef std::future<DeleteBucketTaggingOutcome> DeleteBucketTaggingOutcomeCallable;
      typedef std::future<DeleteBucketWebsiteOutcome> DeleteBucketWebsiteOutcomeCallable;
      typedef std::future<DeleteObjectOutcome> DeleteObjectOutcomeCallable;
      typedef std::future<DeleteObjectTaggingOutcome> DeleteObjectTaggingOutcomeCallable;
      typedef std::future<DeleteObjectsOutcome> DeleteObjectsOutcomeCallable;
      typedef std::future<DeletePublicAccessBlockOutcome> DeletePublicAccessBlockOutcomeCallable;
      typedef std::future<GetBucketAccelerateConfigurationOutcome> GetBucketAccelerateConfigurationOutcomeCallable;
      typedef std::future<GetBucketAclOutcome> GetBucketAclOutcomeCallable;
      typedef std::future<GetBucketAnalyticsConfigurationOutcome> GetBucketAnalyticsConfigurationOutcomeCallable;
      typedef std::future<GetBucketCorsOutcome> GetBucketCorsOutcomeCallable;
      typedef std::future<GetBucketEncryptionOutcome> GetBucketEncryptionOutcomeCallable;
      typedef std::future<GetBucketIntelligentTieringConfigurationOutcome> GetBucketIntelligentTieringConfigurationOutcomeCallable;
      typedef std::future<GetBucketInventoryConfigurationOutcome> GetBucketInventoryConfigurationOutcomeCallable;
      typedef std::future<GetBucketLifecycleConfigurationOutcome> GetBucketLifecycleConfigurationOutcomeCallable;
      typedef std::future<GetBucketLocationOutcome> GetBucketLocationOutcomeCallable;
      typedef std::future<GetBucketLoggingOutcome> GetBucketLoggingOutcomeCallable;
      typedef std::future<GetBucketMetadataTableConfigurationOutcome> GetBucketMetadataTableConfigurationOutcomeCallable;
      typedef std::future<GetBucketMetricsConfigurationOutcome> GetBucketMetricsConfigurationOutcomeCallable;
      typedef std::future<GetBucketNotificationConfigurationOutcome> GetBucketNotificationConfigurationOutcomeCallable;
      typedef std::future<GetBucketOwnershipControlsOutcome> GetBucketOwnershipControlsOutcomeCallable;
      typedef std::future<GetBucketPolicyOutcome> GetBucketPolicyOutcomeCallable;
      typedef std::future<GetBucketPolicyStatusOutcome> GetBucketPolicyStatusOutcomeCallable;
      typedef std::future<GetBucketReplicationOutcome> GetBucketReplicationOutcomeCallable;
      typedef std::future<GetBucketRequestPaymentOutcome> GetBucketRequestPaymentOutcomeCallable;
      typedef std::future<GetBucketTaggingOutcome> GetBucketTaggingOutcomeCallable;
      typedef std::future<GetBucketVersioningOutcome> GetBucketVersioningOutcomeCallable;
      typedef std::future<GetBucketWebsiteOutcome> GetBucketWebsiteOutcomeCallable;
      typedef std::future<GetObjectOutcome> GetObjectOutcomeCallable;
      typedef std::future<GetObjectAclOutcome> GetObjectAclOutcomeCallable;
      typedef std::future<GetObjectAttributesOutcome> GetObjectAttributesOutcomeCallable;
      typedef std::future<GetObjectLegalHoldOutcome> GetObjectLegalHoldOutcomeCallable;
      typedef std::future<GetObjectLockConfigurationOutcome> GetObjectLockConfigurationOutcomeCallable;
      typedef std::future<GetObjectRetentionOutcome> GetObjectRetentionOutcomeCallable;
      typedef std::future<GetObjectTaggingOutcome> GetObjectTaggingOutcomeCallable;
      typedef std::future<GetObjectTorrentOutcome> GetObjectTorrentOutcomeCallable;
      typedef std::future<GetPublicAccessBlockOutcome> GetPublicAccessBlockOutcomeCallable;
      typedef std::future<HeadBucketOutcome> HeadBucketOutcomeCallable;
      typedef std::future<HeadObjectOutcome> HeadObjectOutcomeCallable;
      typedef std::future<ListBucketAnalyticsConfigurationsOutcome> ListBucketAnalyticsConfigurationsOutcomeCallable;
      typedef std::future<ListBucketIntelligentTieringConfigurationsOutcome> ListBucketIntelligentTieringConfigurationsOutcomeCallable;
      typedef std::future<ListBucketInventoryConfigurationsOutcome> ListBucketInventoryConfigurationsOutcomeCallable;
      typedef std::future<ListBucketMetricsConfigurationsOutcome> ListBucketMetricsConfigurationsOutcomeCallable;
      typedef std::future<ListBucketsOutcome> ListBucketsOutcomeCallable;
      typedef std::future<ListDirectoryBucketsOutcome> ListDirectoryBucketsOutcomeCallable;
      typedef std::future<ListMultipartUploadsOutcome> ListMultipartUploadsOutcomeCallable;
      typedef std::future<ListObjectVersionsOutcome> ListObjectVersionsOutcomeCallable;
      typedef std::future<ListObjectsOutcome> ListObjectsOutcomeCallable;
      typedef std::future<ListObjectsV2Outcome> ListObjectsV2OutcomeCallable;
      typedef std::future<ListPartsOutcome> ListPartsOutcomeCallable;
      typedef std::future<PutBucketAccelerateConfigurationOutcome> PutBucketAccelerateConfigurationOutcomeCallable;
      typedef std::future<PutBucketAclOutcome> PutBucketAclOutcomeCallable;
      typedef std::future<PutBucketAnalyticsConfigurationOutcome> PutBucketAnalyticsConfigurationOutcomeCallable;
      typedef std::future<PutBucketCorsOutcome> PutBucketCorsOutcomeCallable;
      typedef std::future<PutBucketEncryptionOutcome> PutBucketEncryptionOutcomeCallable;
      typedef std::future<PutBucketIntelligentTieringConfigurationOutcome> PutBucketIntelligentTieringConfigurationOutcomeCallable;
      typedef std::future<PutBucketInventoryConfigurationOutcome> PutBucketInventoryConfigurationOutcomeCallable;
      typedef std::future<PutBucketLifecycleConfigurationOutcome> PutBucketLifecycleConfigurationOutcomeCallable;
      typedef std::future<PutBucketLoggingOutcome> PutBucketLoggingOutcomeCallable;
      typedef std::future<PutBucketMetricsConfigurationOutcome> PutBucketMetricsConfigurationOutcomeCallable;
      typedef std::future<PutBucketNotificationConfigurationOutcome> PutBucketNotificationConfigurationOutcomeCallable;
      typedef std::future<PutBucketOwnershipControlsOutcome> PutBucketOwnershipControlsOutcomeCallable;
      typedef std::future<PutBucketPolicyOutcome> PutBucketPolicyOutcomeCallable;
      typedef std::future<PutBucketReplicationOutcome> PutBucketReplicationOutcomeCallable;
      typedef std::future<PutBucketRequestPaymentOutcome> PutBucketRequestPaymentOutcomeCallable;
      typedef std::future<PutBucketTaggingOutcome> PutBucketTaggingOutcomeCallable;
      typedef std::future<PutBucketVersioningOutcome> PutBucketVersioningOutcomeCallable;
      typedef std::future<PutBucketWebsiteOutcome> PutBucketWebsiteOutcomeCallable;
      typedef std::future<PutObjectOutcome> PutObjectOutcomeCallable;
      typedef std::future<PutObjectAclOutcome> PutObjectAclOutcomeCallable;
      typedef std::future<PutObjectLegalHoldOutcome> PutObjectLegalHoldOutcomeCallable;
      typedef std::future<PutObjectLockConfigurationOutcome> PutObjectLockConfigurationOutcomeCallable;
      typedef std::future<PutObjectRetentionOutcome> PutObjectRetentionOutcomeCallable;
      typedef std::future<PutObjectTaggingOutcome> PutObjectTaggingOutcomeCallable;
      typedef std::future<PutPublicAccessBlockOutcome> PutPublicAccessBlockOutcomeCallable;
      typedef std::future<RestoreObjectOutcome> RestoreObjectOutcomeCallable;
      typedef std::future<SelectObjectContentOutcome> SelectObjectContentOutcomeCallable;
      typedef std::future<UploadPartOutcome> UploadPartOutcomeCallable;
      typedef std::future<UploadPartCopyOutcome> UploadPartCopyOutcomeCallable;
      typedef std::future<WriteGetObjectResponseOutcome> WriteGetObjectResponseOutcomeCallable;
      /* End of service model Outcome callable definitions */
    } // namespace Model

    class S3Client;

    /* Service model async handlers definitions */
    typedef std::function<void(const S3Client*, const Model::AbortMultipartUploadRequest&, const Model::AbortMultipartUploadOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > AbortMultipartUploadResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::CompleteMultipartUploadRequest&, const Model::CompleteMultipartUploadOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CompleteMultipartUploadResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::CopyObjectRequest&, const Model::CopyObjectOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CopyObjectResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::CreateBucketRequest&, const Model::CreateBucketOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateBucketResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::CreateBucketMetadataTableConfigurationRequest&, const Model::CreateBucketMetadataTableConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateBucketMetadataTableConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::CreateMultipartUploadRequest&, const Model::CreateMultipartUploadOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateMultipartUploadResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::CreateSessionRequest&, const Model::CreateSessionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > CreateSessionResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketRequest&, const Model::DeleteBucketOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketAnalyticsConfigurationRequest&, const Model::DeleteBucketAnalyticsConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketAnalyticsConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketCorsRequest&, const Model::DeleteBucketCorsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketCorsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketEncryptionRequest&, const Model::DeleteBucketEncryptionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketEncryptionResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketIntelligentTieringConfigurationRequest&, const Model::DeleteBucketIntelligentTieringConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketIntelligentTieringConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketInventoryConfigurationRequest&, const Model::DeleteBucketInventoryConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketInventoryConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketLifecycleRequest&, const Model::DeleteBucketLifecycleOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketLifecycleResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketMetadataTableConfigurationRequest&, const Model::DeleteBucketMetadataTableConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketMetadataTableConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketMetricsConfigurationRequest&, const Model::DeleteBucketMetricsConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketMetricsConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketOwnershipControlsRequest&, const Model::DeleteBucketOwnershipControlsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketOwnershipControlsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketPolicyRequest&, const Model::DeleteBucketPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketPolicyResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketReplicationRequest&, const Model::DeleteBucketReplicationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketReplicationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketTaggingRequest&, const Model::DeleteBucketTaggingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketTaggingResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteBucketWebsiteRequest&, const Model::DeleteBucketWebsiteOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteBucketWebsiteResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteObjectRequest&, const Model::DeleteObjectOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteObjectResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteObjectTaggingRequest&, const Model::DeleteObjectTaggingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteObjectTaggingResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeleteObjectsRequest&, const Model::DeleteObjectsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeleteObjectsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::DeletePublicAccessBlockRequest&, const Model::DeletePublicAccessBlockOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > DeletePublicAccessBlockResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketAccelerateConfigurationRequest&, const Model::GetBucketAccelerateConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketAccelerateConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketAclRequest&, const Model::GetBucketAclOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketAclResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketAnalyticsConfigurationRequest&, const Model::GetBucketAnalyticsConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketAnalyticsConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketCorsRequest&, const Model::GetBucketCorsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketCorsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketEncryptionRequest&, const Model::GetBucketEncryptionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketEncryptionResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketIntelligentTieringConfigurationRequest&, const Model::GetBucketIntelligentTieringConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketIntelligentTieringConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketInventoryConfigurationRequest&, const Model::GetBucketInventoryConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketInventoryConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketLifecycleConfigurationRequest&, const Model::GetBucketLifecycleConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketLifecycleConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketLocationRequest&, const Model::GetBucketLocationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketLocationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketLoggingRequest&, const Model::GetBucketLoggingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketLoggingResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketMetadataTableConfigurationRequest&, const Model::GetBucketMetadataTableConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketMetadataTableConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketMetricsConfigurationRequest&, const Model::GetBucketMetricsConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketMetricsConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketNotificationConfigurationRequest&, const Model::GetBucketNotificationConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketNotificationConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketOwnershipControlsRequest&, const Model::GetBucketOwnershipControlsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketOwnershipControlsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketPolicyRequest&, Model::GetBucketPolicyOutcome, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketPolicyResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketPolicyStatusRequest&, const Model::GetBucketPolicyStatusOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketPolicyStatusResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketReplicationRequest&, const Model::GetBucketReplicationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketReplicationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketRequestPaymentRequest&, const Model::GetBucketRequestPaymentOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketRequestPaymentResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketTaggingRequest&, const Model::GetBucketTaggingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketTaggingResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketVersioningRequest&, const Model::GetBucketVersioningOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketVersioningResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetBucketWebsiteRequest&, const Model::GetBucketWebsiteOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetBucketWebsiteResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetObjectRequest&, Model::GetObjectOutcome, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetObjectResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetObjectAclRequest&, const Model::GetObjectAclOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetObjectAclResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetObjectAttributesRequest&, const Model::GetObjectAttributesOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetObjectAttributesResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetObjectLegalHoldRequest&, const Model::GetObjectLegalHoldOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetObjectLegalHoldResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetObjectLockConfigurationRequest&, const Model::GetObjectLockConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetObjectLockConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetObjectRetentionRequest&, const Model::GetObjectRetentionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetObjectRetentionResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetObjectTaggingRequest&, const Model::GetObjectTaggingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetObjectTaggingResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetObjectTorrentRequest&, Model::GetObjectTorrentOutcome, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetObjectTorrentResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::GetPublicAccessBlockRequest&, const Model::GetPublicAccessBlockOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > GetPublicAccessBlockResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::HeadBucketRequest&, const Model::HeadBucketOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > HeadBucketResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::HeadObjectRequest&, const Model::HeadObjectOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > HeadObjectResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListBucketAnalyticsConfigurationsRequest&, const Model::ListBucketAnalyticsConfigurationsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListBucketAnalyticsConfigurationsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListBucketIntelligentTieringConfigurationsRequest&, const Model::ListBucketIntelligentTieringConfigurationsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListBucketIntelligentTieringConfigurationsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListBucketInventoryConfigurationsRequest&, const Model::ListBucketInventoryConfigurationsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListBucketInventoryConfigurationsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListBucketMetricsConfigurationsRequest&, const Model::ListBucketMetricsConfigurationsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListBucketMetricsConfigurationsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListBucketsRequest&, const Model::ListBucketsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListBucketsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListDirectoryBucketsRequest&, const Model::ListDirectoryBucketsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListDirectoryBucketsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListMultipartUploadsRequest&, const Model::ListMultipartUploadsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListMultipartUploadsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListObjectVersionsRequest&, const Model::ListObjectVersionsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListObjectVersionsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListObjectsRequest&, const Model::ListObjectsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListObjectsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListObjectsV2Request&, const Model::ListObjectsV2Outcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListObjectsV2ResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::ListPartsRequest&, const Model::ListPartsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > ListPartsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketAccelerateConfigurationRequest&, const Model::PutBucketAccelerateConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketAccelerateConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketAclRequest&, const Model::PutBucketAclOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketAclResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketAnalyticsConfigurationRequest&, const Model::PutBucketAnalyticsConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketAnalyticsConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketCorsRequest&, const Model::PutBucketCorsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketCorsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketEncryptionRequest&, const Model::PutBucketEncryptionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketEncryptionResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketIntelligentTieringConfigurationRequest&, const Model::PutBucketIntelligentTieringConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketIntelligentTieringConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketInventoryConfigurationRequest&, const Model::PutBucketInventoryConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketInventoryConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketLifecycleConfigurationRequest&, const Model::PutBucketLifecycleConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketLifecycleConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketLoggingRequest&, const Model::PutBucketLoggingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketLoggingResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketMetricsConfigurationRequest&, const Model::PutBucketMetricsConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketMetricsConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketNotificationConfigurationRequest&, const Model::PutBucketNotificationConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketNotificationConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketOwnershipControlsRequest&, const Model::PutBucketOwnershipControlsOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketOwnershipControlsResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketPolicyRequest&, const Model::PutBucketPolicyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketPolicyResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketReplicationRequest&, const Model::PutBucketReplicationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketReplicationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketRequestPaymentRequest&, const Model::PutBucketRequestPaymentOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketRequestPaymentResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketTaggingRequest&, const Model::PutBucketTaggingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketTaggingResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketVersioningRequest&, const Model::PutBucketVersioningOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketVersioningResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutBucketWebsiteRequest&, const Model::PutBucketWebsiteOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutBucketWebsiteResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutObjectRequest&, const Model::PutObjectOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutObjectResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutObjectAclRequest&, const Model::PutObjectAclOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutObjectAclResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutObjectLegalHoldRequest&, const Model::PutObjectLegalHoldOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutObjectLegalHoldResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutObjectLockConfigurationRequest&, const Model::PutObjectLockConfigurationOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutObjectLockConfigurationResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutObjectRetentionRequest&, const Model::PutObjectRetentionOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutObjectRetentionResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutObjectTaggingRequest&, const Model::PutObjectTaggingOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutObjectTaggingResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::PutPublicAccessBlockRequest&, const Model::PutPublicAccessBlockOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > PutPublicAccessBlockResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::RestoreObjectRequest&, const Model::RestoreObjectOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > RestoreObjectResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::SelectObjectContentRequest&, const Model::SelectObjectContentOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > SelectObjectContentResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::UploadPartRequest&, const Model::UploadPartOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UploadPartResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::UploadPartCopyRequest&, const Model::UploadPartCopyOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > UploadPartCopyResponseReceivedHandler;
    typedef std::function<void(const S3Client*, const Model::WriteGetObjectResponseRequest&, const Model::WriteGetObjectResponseOutcome&, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&) > WriteGetObjectResponseResponseReceivedHandler;
    /* End of service model async handlers definitions */
  } // namespace S3
} // namespace Aws
