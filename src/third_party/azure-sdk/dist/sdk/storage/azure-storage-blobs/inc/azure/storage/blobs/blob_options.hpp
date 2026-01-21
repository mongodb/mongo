// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/blobs/rest_client.hpp"

#include <azure/core/internal/client_options.hpp>
#include <azure/core/internal/extendable_enumeration.hpp>
#include <azure/core/match_conditions.hpp>
#include <azure/core/modified_conditions.hpp>
#include <azure/storage/common/access_conditions.hpp>
#include <azure/storage/common/crypt.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace Azure { namespace Storage { namespace Blobs {

  /**
   * @brief Audiences available for blob service
   *
   */
  class BlobAudience final : public Azure::Core::_internal::ExtendableEnumeration<BlobAudience> {
  public:
    /**
     * @brief Construct a new BlobAudience object
     *
     * @param blobAudience The Azure Active Directory audience to use when forming authorization
     * scopes. For the Language service, this value corresponds to a URL that identifies the Azure
     * cloud where the resource is located. For more information: See
     * https://learn.microsoft.com/en-us/azure/storage/blobs/authorize-access-azure-active-directory
     */
    explicit BlobAudience(std::string blobAudience) : ExtendableEnumeration(std::move(blobAudience))
    {
    }

    /**
     * @brief The service endpoint for a given storage account. Use this method to acquire a token
     * for authorizing requests to that specific Azure Storage account and service only.
     *
     * @param storageAccountName he storage account name used to populate the service endpoint.
     * @return The service endpoint for a given storage account.
     */
    static BlobAudience CreateBlobServiceAccountAudience(const std::string& storageAccountName)
    {
      return BlobAudience("https://" + storageAccountName + ".blob.core.windows.net/");
    }

    /**
     * @brief Default Audience. Use to acquire a token for authorizing requests to any Azure
     * Storage account.
     */
    AZ_STORAGE_BLOBS_DLLEXPORT const static BlobAudience DefaultAudience;
  };

  /**
   * @brief Specifies access conditions for a container.
   */
  struct BlobContainerAccessConditions final : public Azure::ModifiedConditions,
                                               public LeaseAccessConditions
  {
  };

  /**
   * @brief Specifies HTTP options for conditional requests based on tags.
   */
  struct TagAccessConditions
  {
    /**
     * @brief Destructor.
     *
     */
    virtual ~TagAccessConditions() = default;

    /**
     * @brief Optional SQL statement to apply to the tags of the Blob. Refer to
     * https://docs.microsoft.com/rest/api/storageservices/specifying-conditional-headers-for-blob-service-operations#tags-predicate-syntax
     * for the format of SQL statements.
     */
    Azure::Nullable<std::string> TagConditions;
  };

  /**
   * @brief Specifies access conditions for a blob.
   */
  struct BlobAccessConditions : public Azure::ModifiedConditions,
                                public Azure::MatchConditions,
                                public LeaseAccessConditions,
                                public TagAccessConditions
  {
  };

  /**
   * @brief Specifies access conditions for blob lease operations.
   */
  struct LeaseBlobAccessConditions final : public Azure::ModifiedConditions,
                                           public Azure::MatchConditions,
                                           public TagAccessConditions
  {
  };

  /**
   * @brief Specifies access conditions for an append blob.
   */
  struct AppendBlobAccessConditions final : public BlobAccessConditions
  {
    /**
     * @brief Ensures that the AppendBlock operation succeeds only if the append blob's size
     * is less than or equal to this value.
     */
    Azure::Nullable<int64_t> IfMaxSizeLessThanOrEqual;

    /**
     * @brief Ensures that the AppendBlock operation succeeds only if the append position is equal
     * to this value.
     */
    Azure::Nullable<int64_t> IfAppendPositionEqual;
  };

  /**
   * @brief Specifies access conditions for a page blob.
   */
  struct PageBlobAccessConditions final : public BlobAccessConditions
  {
    /**
     * @brief IfSequenceNumberLessThan ensures that the page blob operation succeeds only if
     * the blob's sequence number is less than a value.
     */
    Azure::Nullable<int64_t> IfSequenceNumberLessThan;

    /**
     * @brief IfSequenceNumberLessThanOrEqual ensures that the page blob operation succeeds
     * only if the blob's sequence number is less than or equal to a value.
     */
    Azure::Nullable<int64_t> IfSequenceNumberLessThanOrEqual;

    /**
     * @brief IfSequenceNumberEqual ensures that the page blob operation succeeds only
     * if the blob's sequence number is equal to a value.
     */
    Azure::Nullable<int64_t> IfSequenceNumberEqual;
  };

  /**
   * @brief Wrapper for an encryption key to be used with client provided key server-side
   * encryption.
   */
  struct EncryptionKey final
  {
    /**
     * @brief Base64 encoded string of the AES256 encryption key.
     */
    std::string Key;

    /**
     * @brief SHA256 hash of the AES256 encryption key.
     */
    std::vector<uint8_t> KeyHash;

    /**
     * @brief The algorithm for Azure Blob Storage to encrypt with.
     */
    Models::EncryptionAlgorithmType Algorithm;
  };

  /**
   * @brief Client options used to initialize all kinds of blob clients.
   */
  struct BlobClientOptions final : Azure::Core::_internal::ClientOptions
  {
    /**
     * @brief Holds the customer provided key used when making requests.
     */
    Azure::Nullable<EncryptionKey> CustomerProvidedKey;

    /**
     * @brief Holds the encryption scope used when making requests.
     */
    Azure::Nullable<std::string> EncryptionScope;

    /**
     * SecondaryHostForRetryReads specifies whether the retry policy should retry a read
     * operation against another host. If SecondaryHostForRetryReads is "" (the default) then
     * operations are not retried against another host. NOTE: Before setting this field, make sure
     * you understand the issues around reading stale & potentially-inconsistent data at this
     * webpage: https://docs.microsoft.com/azure/storage/common/geo-redundant-design.
     */
    std::string SecondaryHostForRetryReads;

    /**
     * API version used by this client.
     */
    std::string ApiVersion;

    /**
     * Enables tenant discovery through the authorization challenge when the client is configured to
     * use a TokenCredential. When enabled, the client will attempt an initial un-authorized request
     * to prompt a challenge in order to discover the correct tenant for the resource.
     */
    bool EnableTenantDiscovery = false;

    /**
     * The Audience to use for authentication with Azure Active Directory (AAD).
     * #Azure::Storage::Blobs::BlobAudience::DefaultAudience will be assumed if Audience is
     * not set.
     */
    Azure::Nullable<BlobAudience> Audience;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobServiceClient::ListBlobContainers.
   */
  struct ListBlobContainersOptions final
  {
    /**
     * @brief Specifies a string that filters the results to return only containers whose
     * name begins with the specified prefix.
     */
    Azure::Nullable<std::string> Prefix;

    /**
     * @brief A string value that identifies the portion of the list of containers to be
     * returned with the next listing operation. The operation returns a non-empty
     * ListBlobContainersSegment.ContinuationToken value if the listing operation did not return all
     * containers remaining to be listed with the current segment. The ContinuationToken value can
     * be used as the value for the ContinuationToken parameter in a subsequent call to request the
     * next segment of list items.
     */
    Azure::Nullable<std::string> ContinuationToken;

    /**
     * @brief Specifies the maximum number of containers to return.
     */
    Azure::Nullable<int32_t> PageSizeHint;

    /**
     * @brief Specifies that the container's metadata be returned.
     */
    Models::ListBlobContainersIncludeFlags Include = Models::ListBlobContainersIncludeFlags::None;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobServiceClient::GetUserDelegationKey.
   */
  struct GetUserDelegationKeyOptions final
  {
    /**
     * @brief Start time for the key's validity. The time should be specified in UTC, and
     * will be truncated to second.
     */
    Azure::DateTime StartsOn = std::chrono::system_clock::now();
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobServiceClient::SetProperties.
   */
  struct SetServicePropertiesOptions final
  {
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobServiceClient::GetProperties.
   */
  struct GetServicePropertiesOptions final
  {
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobServiceClient::GetAccountInfo.
   */
  struct GetAccountInfoOptions final
  {
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobServiceClient::GetStatistics.
   */
  struct GetBlobServiceStatisticsOptions final
  {
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobServiceClient::FindBlobsByTags.
   */
  struct FindBlobsByTagsOptions final
  {
    /**
     * @brief A string value that identifies the portion of the result set to be returned
     * with the next operation. The operation returns a ContinuationToken value within the response
     * body if the result set returned was not complete. The ContinuationToken value may then be
     * used in a subsequent call to request the next set of items..
     */
    Azure::Nullable<std::string> ContinuationToken;

    /**
     * @brief Specifies the maximum number of blobs to return.
     */
    Azure::Nullable<int32_t> PageSizeHint;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobContainerClient::Create.
   */
  struct CreateBlobContainerOptions final
  {
    /**
     * @brief Specifies whether data in the container may be accessed publicly and the level
     * of access.
     */
    Models::PublicAccessType AccessType = Models::PublicAccessType::None;

    /**
     * @brief Name-value pairs to associate with the container as metadata.
     */
    Storage::Metadata Metadata;

    /**
     * @brief The encryption scope to use as the default on the container.
     */
    Azure::Nullable<std::string> DefaultEncryptionScope;

    /**
     * @brief If true, prevents any blob upload from specifying a different encryption
     * scope.
     */
    Azure::Nullable<bool> PreventEncryptionScopeOverride;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobContainerClient::Delete.
   */
  struct DeleteBlobContainerOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobContainerAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for
   * #Azure::Storage::Blobs::BlobServiceClient::UndeleteBlobContainer.
   */
  struct UndeleteBlobContainerOptions final
  {
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobServiceClient::RenameBlobContainer.
   */
  struct RenameBlobContainerOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseAccessConditions SourceAccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobContainerClient::GetProperties.
   */
  struct GetBlobContainerPropertiesOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobContainerClient::SetMetadata.
   */
  struct SetBlobContainerMetadataOptions final
  {
    struct : public LeaseAccessConditions
    {
      /**
       * @brief Specify this header to perform the operation only if the resource has been
       * modified since the specified time. This timestamp will be truncated to second.
       */
      Azure::Nullable<Azure::DateTime> IfModifiedSince;
    } /**
       * @brief Optional conditions that must be met to perform this operation.
       */
    AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobContainerClient::ListBlobs and
   * #Azure::Storage::Blobs::BlobContainerClient::ListBlobsByHierarchy.
   */
  struct ListBlobsOptions final
  {
    /**
     * @brief Specifies a string that filters the results to return only blobs whose
     * name begins with the specified prefix.
     */
    Azure::Nullable<std::string> Prefix;

    /**
     * @brief A string value that identifies the portion of the list of blobs to be
     * returned with the next listing operation. The operation returns a non-empty
     * BlobsFlatSegment.ContinuationToken value if the listing operation did not return all blobs
     * remaining to be listed with the current segment. The ContinuationToken value can be used as
     * the value for the ContinuationToken parameter in a subsequent call to request the next
     * segment of list items.
     */
    Azure::Nullable<std::string> ContinuationToken;

    /**
     * @brief Specifies the maximum number of blobs to return.
     */
    Azure::Nullable<int32_t> PageSizeHint;

    /**
     * @brief Specifies one or more datasets to include in the response.
     */
    Models::ListBlobsIncludeFlags Include = Models::ListBlobsIncludeFlags::None;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobContainerClient::GetAccessPolicy.
   */
  struct GetBlobContainerAccessPolicyOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobContainerClient::SetAccessPolicy.
   */
  struct SetBlobContainerAccessPolicyOptions final
  {
    /**
     * @brief Specifies whether data in the container may be accessed publicly and the level
     * of access.
     */
    Models::PublicAccessType AccessType = Models::PublicAccessType::None;

    /**
     * @brief Stored access policies that you can use to provide fine grained control over
     * container permissions.
     */
    std::vector<Models::SignedIdentifier> SignedIdentifiers;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobContainerAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::GetProperties.
   */
  struct GetBlobPropertiesOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::SetHttpHeaders.
   */
  struct SetBlobHttpHeadersOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::SetMetadata.
   */
  struct SetBlobMetadataOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::SetAccessTier.
   */
  struct SetBlobAccessTierOptions final
  {
    /**
     * @brief Indicates the priority with which to rehydrate an archived blob. The priority
     * can be set on a blob only once. This header will be ignored on subsequent requests to the
     * same blob.
     */
    Azure::Nullable<Models::RehydratePriority> RehydratePriority;
    struct : public LeaseAccessConditions, public TagAccessConditions
    {
    } /**
       * @brief Optional conditions that must be met to perform this operation.
       */
    AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::StartCopyFromUri.
   */
  struct StartBlobCopyFromUriOptions final
  {
    /**
     * @brief Specifies user-defined name-value pairs associated with the blob. If no
     * name-value pairs are specified, the operation will copy the metadata from the source blob or
     * file to the destination blob. If one or more name-value pairs are specified, the destination
     * blob is created with the specified metadata, and metadata is not copied from the source blob
     * or file.
     */
    Storage::Metadata Metadata;

    /**
     * @brief The tags to set for this blob.
     */
    std::map<std::string, std::string> Tags;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;

    struct : public Azure::ModifiedConditions,
             public Azure::MatchConditions,
             public LeaseAccessConditions,
             public TagAccessConditions
    {
    } /**
       * @brief Optional conditions that the source must meet to perform this operation.
       *
       * @note Lease access condition only works for API versions before 2012-02-12.
       */
    SourceAccessConditions;

    /**
     * @brief Specifies the tier to be set on the target blob.
     */
    Azure::Nullable<Models::AccessTier> AccessTier;

    /**
     * @brief Indicates the priority with which to rehydrate an archived blob. The priority
     * can be set on a blob only once. This header will be ignored on subsequent requests to the
     * same blob.
     */
    Azure::Nullable<Models::RehydratePriority> RehydratePriority;

    /**
     * @brief If the destination blob should be sealed. Only applicable for Append Blobs.
     */
    Azure::Nullable<bool> ShouldSealDestination;

    /**
     * Immutability policy to set on the destination blob.
     */
    Azure::Nullable<Models::BlobImmutabilityPolicy> ImmutabilityPolicy;

    /**
     * Indicates whether the destination blob has a legal hold.
     */
    Azure::Nullable<bool> HasLegalHold;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::CopyFromUri.
   */
  struct CopyBlobFromUriOptions
  {
    /**
     * @brief Specifies user-defined name-value pairs associated with the blob. If no
     * name-value pairs are specified, the operation will copy the metadata from the source blob or
     * file to the destination blob. If one or more name-value pairs are specified, the destination
     * blob is created with the specified metadata, and metadata is not copied from the source blob
     * or file.
     */
    Storage::Metadata Metadata;

    /**
     * @brief The tags to set for this blob.
     */
    std::map<std::string, std::string> Tags;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;

    struct : public Azure::ModifiedConditions, public Azure::MatchConditions
    {
    } /**
       * @brief Optional conditions that the source must meet to perform this operation.
       *
       * @note Lease access condition only works for API versions before 2012-02-12.
       */
    SourceAccessConditions;

    /**
     * @brief Specifies the tier to be set on the target blob.
     */
    Azure::Nullable<Models::AccessTier> AccessTier;

    /**
     * @brief Hash of the blob content. This hash is used to verify the integrity of
     * the blob during transport. When this header is specified, the storage service checks the hash
     * that has arrived with the one that was sent.
     */
    Azure::Nullable<ContentHash> TransactionalContentHash;

    /**
     * Immutability policy to set on the destination blob.
     */
    Azure::Nullable<Models::BlobImmutabilityPolicy> ImmutabilityPolicy;
    /**
     * Indicates whether the destination blob has a legal hold.
     */
    Azure::Nullable<bool> HasLegalHold;

    /**
     * Indicates the tags on the destination blob should be copied from source or replaced by Tags
     * in this option. Default is to replace.
     */
    Models::BlobCopySourceTagsMode CopySourceTagsMode;

    /**
     * @brief Optional. Source authorization used to access the source file.
     * The format is: \<scheme\> \<signature\>
     * Only Bearer type is supported. Credentials should be a valid OAuth access token to copy
     * source.
     */
    std::string SourceAuthorization;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::AbortCopyFromUri.
   */
  struct AbortBlobCopyFromUriOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::Download.
   */
  struct DownloadBlobOptions final
  {
    /**
     * @brief Downloads only the bytes of the blob in the specified range.
     */
    Azure::Nullable<Core::Http::HttpRange> Range;

    /**
     * @brief When specified together with Range, service returns hash for the range as long as the
     * range is less than or equal to 4 MiB in size.
     */
    Azure::Nullable<HashAlgorithm> RangeHashAlgorithm;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::DownloadTo.
   */
  struct DownloadBlobToOptions final
  {
    /**
     * @brief Downloads only the bytes of the blob in the specified range.
     */
    Azure::Nullable<Core::Http::HttpRange> Range;

    /**
     * @brief Options for parallel transfer.
     */
    struct
    {
      /**
       * @brief The size of the first range request in bytes. Blobs smaller than this limit will be
       * downloaded in a single request. Blobs larger than this limit will continue being downloaded
       * in chunks of size ChunkSize.
       */
      int64_t InitialChunkSize = 256 * 1024 * 1024;

      /**
       * @brief The maximum number of bytes in a single request.
       */
      int64_t ChunkSize = 4 * 1024 * 1024;

      /**
       * @brief The maximum number of threads that may be used in a parallel transfer.
       */
      int32_t Concurrency = 5;
    } TransferOptions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::CreateSnapshot.
   */
  struct CreateBlobSnapshotOptions final
  {
    /**
     * @brief Specifies user-defined name-value pairs associated with the blob. If no
     * name-value pairs are specified, the operation will copy the base blob metadata to the
     * snapshot. If one or more name-value pairs are specified, the snapshot is created with the
     * specified metadata, and metadata is not copied from the base blob.
     */
    Storage::Metadata Metadata;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::Delete.
   */
  struct DeleteBlobOptions final
  {
    /**
     * @brief Specifies to delete either the base blob
     * and all of its snapshots, or only the blob's snapshots and not the blob itself. Required if
     * the blob has associated snapshots.
     */
    Azure::Nullable<Models::DeleteSnapshotsOption> DeleteSnapshots;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::Undelete.
   */
  struct UndeleteBlobOptions final
  {
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobLeaseClient::Acquire.
   */
  struct AcquireLeaseOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseBlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobLeaseClient::Renew.
   */
  struct RenewLeaseOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseBlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobLeaseClient::Change.
   */
  struct ChangeLeaseOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseBlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobLeaseClient::Release.
   */
  struct ReleaseLeaseOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseBlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobLeaseClient::Break.
   */
  struct BreakLeaseOptions final
  {
    /**
     * @brief Proposed duration the lease should continue before it is broken, in seconds,
     * between 0 and 60. This break period is only used if it is shorter than the time remaining on
     * the lease. If longer, the time remaining on the lease is used. A new lease will not be
     * available before the break period has expired, but the lease may be held for longer than the
     * break period.
     */
    Azure::Nullable<std::chrono::seconds> BreakPeriod;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseBlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::SetTags.
   */
  struct SetBlobTagsOptions final
  {
    struct : public LeaseAccessConditions, public TagAccessConditions
    {
    } /**
       * @brief Optional conditions that must be met to perform this operation.
       */
    AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::GetTags.
   */
  struct GetBlobTagsOptions final
  {
    struct : public LeaseAccessConditions, public TagAccessConditions
    {
    } /**
       * @brief Optional conditions that must be met to perform this operation.
       */
    AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlockBlobClient::Upload.
   */
  struct UploadBlockBlobOptions final
  {
    /**
     * @brief Hash of the blob content. This hash is used to verify the integrity of
     * the blob during transport. When this header is specified, the storage service checks the hash
     * that has arrived with the one that was sent.
     */
    Azure::Nullable<ContentHash> TransactionalContentHash;

    /**
     * @brief The standard HTTP header system properties to set.
     */
    Models::BlobHttpHeaders HttpHeaders;

    /**
     * @brief Name-value pairs associated with the blob as metadata.
     */
    Storage::Metadata Metadata;

    /**
     * @brief The tags to set for this blob.
     */
    std::map<std::string, std::string> Tags;

    /**
     * @brief Indicates the tier to be set on blob.
     */
    Azure::Nullable<Models::AccessTier> AccessTier;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;

    /**
     * Immutability policy to set on the blob.
     */
    Azure::Nullable<Models::BlobImmutabilityPolicy> ImmutabilityPolicy;

    /**
     * Indicates whether the blob has a legal hold.
     */
    Azure::Nullable<bool> HasLegalHold;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlockBlobClient::UploadFrom.
   */
  struct UploadBlockBlobFromOptions final
  {
    /**
     * @brief The standard HTTP header system properties to set.
     */
    Models::BlobHttpHeaders HttpHeaders;

    /**
     * @brief Name-value pairs associated with the blob as metadata.
     */
    Storage::Metadata Metadata;

    /**
     * @brief The tags to set for this blob.
     */
    std::map<std::string, std::string> Tags;

    /**
     * @brief Indicates the tier to be set on blob.
     */
    Azure::Nullable<Models::AccessTier> AccessTier;

    /**
     * @brief Options for parallel transfer.
     */
    struct
    {
      /**
       * @brief Blob smaller than this will be uploaded with a single upload operation. This value
       * cannot be larger than 5000 MiB.
       */
      int64_t SingleUploadThreshold = 256 * 1024 * 1024;

      /**
       * @brief The maximum number of bytes in a single request. This value cannot be larger than
       * 4000 MiB.
       */
      Azure::Nullable<int64_t> ChunkSize;

      /**
       * @brief The maximum number of threads that may be used in a parallel transfer.
       */
      int32_t Concurrency = 5;
    } TransferOptions;

    /**
     * Immutability policy to set on the blob.
     */
    Azure::Nullable<Models::BlobImmutabilityPolicy> ImmutabilityPolicy;

    /**
     * Indicates whether the blob has a legal hold.
     */
    Azure::Nullable<bool> HasLegalHold;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlockBlobClient::UploadFromUri.
   */
  struct UploadBlockBlobFromUriOptions final
  {
    /**
     * If true, the properties of the source blob will be copied to the new blob.
     */
    bool CopySourceBlobProperties = true;

    /**
     * @brief The standard HTTP header system properties to set.
     */
    Models::BlobHttpHeaders HttpHeaders;

    /**
     * @brief Name-value pairs associated with the blob as metadata.
     */
    Storage::Metadata Metadata;

    /**
     * @brief The tags to set for this blob.
     */
    std::map<std::string, std::string> Tags;

    /**
     * @brief Indicates the tier to be set on blob.
     */
    Azure::Nullable<Models::AccessTier> AccessTier;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;

    struct : public Azure::ModifiedConditions,
             public Azure::MatchConditions,
             public TagAccessConditions
    {
    } /**
       * @brief Optional conditions that source must meet to perform this operation.
       * @remarks Azure storage service doesn't support tags access condition for this operation.
       * Don't use it.
       */
    SourceAccessConditions;

    /**
     * @brief Hash of the blob content. This hash is used to verify the integrity of
     * the blob during transport. When this header is specified, the storage service checks the hash
     * that has arrived with the one that was sent. Note that this hash is not stored with the blob.
     * If the two hashes do not match, the operation will fail.
     */
    Azure::Nullable<ContentHash> TransactionalContentHash;

    /**
     * Indicates the tags on the destination blob should be copied from source or replaced by Tags
     * in this option. Default is to replace.
     */
    Models::BlobCopySourceTagsMode CopySourceTagsMode;

    /**
     * @brief Optional. Source authorization used to access the source file.
     * The format is: \<scheme\> \<signature\>
     * Only Bearer type is supported. Credentials should be a valid OAuth access token to copy
     * source.
     */
    std::string SourceAuthorization;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlockBlobClient::StageBlock.
   */
  struct StageBlockOptions final
  {
    /**
     * @brief Hash of the blob content. This hash is used to verify the integrity of
     * the blob during transport. When this header is specified, the storage service checks the hash
     * that has arrived with the one that was sent.
     */
    Azure::Nullable<ContentHash> TransactionalContentHash;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlockBlobClient::StageBlockFromUri.
   */
  struct StageBlockFromUriOptions final
  {
    /**
     * @brief Uploads only the bytes of the source blob in the specified range.
     */
    Azure::Nullable<Core::Http::HttpRange> SourceRange;

    /**
     * @brief Hash of the blob content. This hash is used to verify the integrity of
     * the blob during transport. When this header is specified, the storage service checks the hash
     * that has arrived with the one that was sent.
     */
    Azure::Nullable<ContentHash> TransactionalContentHash;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    LeaseAccessConditions AccessConditions;

    struct : public Azure::ModifiedConditions, public Azure::MatchConditions
    {
    } /**
       * @brief Optional conditions that the source must meet to perform this operation.
       */
    SourceAccessConditions;

    /**
     * @brief Optional. Source authorization used to access the source file.
     * The format is: \<scheme\> \<signature\>
     * Only Bearer type is supported. Credentials should be a valid OAuth access token to copy
     * source.
     */
    std::string SourceAuthorization;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlockBlobClient::CommitBlockList.
   */
  struct CommitBlockListOptions final
  {
    /**
     * @brief The standard HTTP header system properties to set.
     */
    Models::BlobHttpHeaders HttpHeaders;

    /**
     * @brief Name-value pairs associated with the blob as metadata.
     */
    Storage::Metadata Metadata;

    /**
     * @brief The tags to set for this blob.
     */
    std::map<std::string, std::string> Tags;

    /**
     * @brief Indicates the tier to be set on blob.
     */
    Azure::Nullable<Models::AccessTier> AccessTier;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;

    /**
     * Immutability policy to set on the blob.
     */
    Azure::Nullable<Models::BlobImmutabilityPolicy> ImmutabilityPolicy;

    /**
     * Indicates whether the blob has a legal hold.
     */
    Azure::Nullable<bool> HasLegalHold;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlockBlobClient::GetBlockList.
   */
  struct GetBlockListOptions final
  {
    /**
     * @brief Specifies whether to return the list of committed blocks, the list of uncommitted
     * blocks, or both lists together.
     */
    Models::BlockListType ListType = Models::BlockListType::Committed;

    struct : public LeaseAccessConditions, public TagAccessConditions
    {
    } /**
       * @brief Optional conditions that must be met to perform this operation.
       */
    AccessConditions;
  };

  /**
   * @brief Blob Query text configuration for input.
   */
  class BlobQueryInputTextOptions final {
  public:
    /**
     * @brief Creates CSV text configuration.
     *
     * @param recordSeparator Record separator.
     * @param columnSeparator Column separator.
     * @param quotationCharacter Field quote.
     * @param escapeCharacter Escape character.
     * @param hasHeaders If CSV file has headers.
     * @return CSV text configuration.
     */
    static BlobQueryInputTextOptions CreateCsvTextOptions(
        const std::string& recordSeparator = std::string(),
        const std::string& columnSeparator = std::string(),
        const std::string& quotationCharacter = std::string(),
        const std::string& escapeCharacter = std::string(),
        bool hasHeaders = false);
    /**
     * @brief Creates Json text configuration.
     *
     * @param recordSeparator Record separator.
     * @return Json text configuration.
     */
    static BlobQueryInputTextOptions CreateJsonTextOptions(
        const std::string& recordSeparator = std::string());
    /**
     * @brief Creates Parquet text configuration.
     *
     * @return Parquet text configuration
     */
    static BlobQueryInputTextOptions CreateParquetTextOptions();

  private:
    Models::_detail::QueryFormatType m_format;
    std::string m_recordSeparator;
    std::string m_columnSeparator;
    std::string m_quotationCharacter;
    std::string m_escapeCharacter;
    bool m_hasHeaders = false;

    friend class BlockBlobClient;
  };

  /**
   * @brief Blob Query text configuration for output.
   */
  class BlobQueryOutputTextOptions final {
  public:
    /**
     * @brief Creates CSV text configuration.
     *
     * @param recordSeparator Record separator.
     * @param columnSeparator Column separator.
     * @param quotationCharacter Field quote.
     * @param escapeCharacter Escape character.
     * @param hasHeaders If CSV file has headers.
     * @return CSV text configuration.
     */
    static BlobQueryOutputTextOptions CreateCsvTextOptions(
        const std::string& recordSeparator = std::string(),
        const std::string& columnSeparator = std::string(),
        const std::string& quotationCharacter = std::string(),
        const std::string& escapeCharacter = std::string(),
        bool hasHeaders = false);
    /**
     * @brief Creates Json text configuration.
     *
     * @param recordSeparator Record separator.
     * @return Json text configuration.
     */
    static BlobQueryOutputTextOptions CreateJsonTextOptions(
        const std::string& recordSeparator = std::string());
    /**
     * @brief Creates Arrow text configuration.
     *
     * @param schema A list of fields describing the schema.
     * @return Arrow text configuration.
     */
    static BlobQueryOutputTextOptions CreateArrowTextOptions(
        std::vector<Models::BlobQueryArrowField> schema);

  private:
    Models::_detail::QueryFormatType m_format;
    std::string m_recordSeparator;
    std::string m_columnSeparator;
    std::string m_quotationCharacter;
    std::string m_escapeCharacter;
    bool m_hasHeaders = false;
    std::vector<Models::BlobQueryArrowField> m_schema;

    friend class BlockBlobClient;
  };

  /**
   * @brief Blob Query Error.
   */
  struct BlobQueryError final
  {
    /**
     * @brief Error name.
     */
    std::string Name;
    /**
     * @brief Error description.
     */
    std::string Description;
    /**
     * @brief If the error is a fatal error.
     */
    bool IsFatal = false;
    /**
     * The position of the error..
     */
    int64_t Position;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlockBlobClient::Query.
   */
  struct QueryBlobOptions final
  {
    /**
     * @brief Input text configuration.
     */
    BlobQueryInputTextOptions InputTextConfiguration;
    /**
     * @brief Output text configuration.
     */
    BlobQueryOutputTextOptions OutputTextConfiguration;
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;
    /**
     * @brief Callback for progress handling.
     */
    std::function<void(int64_t, int64_t)> ProgressHandler;
    /**
     * @brief Callback for error handling. If you don't specify one, the default will be used, which
     * will ignore all non-fatal errors and throw for fatal errors.
     */
    std::function<void(BlobQueryError)> ErrorHandler;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::AppendBlobClient::Create.
   */
  struct CreateAppendBlobOptions final
  {
    /**
     * @brief The standard HTTP header system properties to set.
     */
    Models::BlobHttpHeaders HttpHeaders;

    /**
     * @brief Name-value pairs associated with the blob as metadata.
     */
    Storage::Metadata Metadata;

    /**
     * @brief The tags to set for this blob.
     */
    std::map<std::string, std::string> Tags;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;

    /**
     * Immutability policy to set on the blob.
     */
    Azure::Nullable<Models::BlobImmutabilityPolicy> ImmutabilityPolicy;

    /**
     * Indicates whether the blob has a legal hold.
     */
    Azure::Nullable<bool> HasLegalHold;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::AppendBlobClient::AppendBlock.
   */
  struct AppendBlockOptions final
  {
    /**
     * @brief Hash of the blob content. This hash is used to verify the integrity of
     * the blob during transport. When this header is specified, the storage service checks the hash
     * that has arrived with the one that was sent.
     */
    Azure::Nullable<ContentHash> TransactionalContentHash;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    AppendBlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::AppendBlobClient::AppendBlockFromUri.
   */
  struct AppendBlockFromUriOptions final
  {
    /**
     * @brief Uploads only the bytes of the source blob in the specified range.
     */
    Azure::Nullable<Core::Http::HttpRange> SourceRange;

    /**
     * @brief Hash of the blob content. This hash is used to verify the integrity of
     * the blob during transport. When this header is specified, the storage service checks the hash
     * that has arrived with the one that was sent.
     */
    Azure::Nullable<ContentHash> TransactionalContentHash;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    AppendBlobAccessConditions AccessConditions;

    /**
     * @brief Optional. Source authorization used to access the source file.
     * The format is: \<scheme\> \<signature\>
     * Only Bearer type is supported. Credentials should be a valid OAuth access token to copy
     * source.
     */
    std::string SourceAuthorization;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::AppendBlobClient::Seal.
   */
  struct SealAppendBlobOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     * @remarks Azure storage service doesn't support tags access condition for this operation.
     * Don't use it.
     */
    AppendBlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::PageBlobClient::Create.
   */
  struct CreatePageBlobOptions final
  {
    /**
     * @brief The sequence number is a user-controlled value that you can use to track requests. The
     * value of the sequence number must be between 0 and 2^63 - 1.
     */
    Azure::Nullable<int64_t> SequenceNumber;

    /**
     * @brief The standard HTTP header system properties to set.
     */
    Models::BlobHttpHeaders HttpHeaders;

    /**
     * @brief Name-value pairs associated with the blob as metadata.
     */
    Storage::Metadata Metadata;

    /**
     * @brief Indicates the tier to be set on blob.
     */
    Azure::Nullable<Models::AccessTier> AccessTier;

    /**
     * @brief The tags to set for this blob.
     */
    std::map<std::string, std::string> Tags;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;

    /**
     * Immutability policy to set on the blob.
     */
    Azure::Nullable<Models::BlobImmutabilityPolicy> ImmutabilityPolicy;

    /**
     * Indicates whether the blob has a legal hold.
     */
    Azure::Nullable<bool> HasLegalHold;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::PageBlobClient::UploadPages.
   */
  struct UploadPagesOptions final
  {
    /**
     * @brief Hash of the blob content. This hash is used to verify the integrity of
     * the blob during transport. When this header is specified, the storage service checks the hash
     * that has arrived with the one that was sent.
     */
    Azure::Nullable<ContentHash> TransactionalContentHash;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    PageBlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::PageBlobClient::UploadPagesFromUri.
   */
  struct UploadPagesFromUriOptions final
  {
    /**
     * @brief Hash of the blob content. This hash is used to verify the integrity of
     * the blob during transport. When this header is specified, the storage service checks the hash
     * that has arrived with the one that was sent.
     */
    Azure::Nullable<ContentHash> TransactionalContentHash;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    PageBlobAccessConditions AccessConditions;

    struct : public Azure::ModifiedConditions, public Azure::MatchConditions
    {
    } /**
       * @brief Optional conditions that the source must meet to perform this operation.
       */
    SourceAccessConditions;

    /**
     * @brief Optional. Source authorization used to access the source file.
     * The format is: \<scheme\> \<signature\>
     * Only Bearer type is supported. Credentials should be a valid OAuth access token to copy
     * source.
     */
    std::string SourceAuthorization;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::PageBlobClient::ClearPages.
   */
  struct ClearPagesOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    PageBlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::PageBlobClient::Resize.
   */
  struct ResizePageBlobOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::PageBlobClient::UpdateSequenceNumber.
   */
  struct UpdatePageBlobSequenceNumberOptions final
  {
    /**
     * @brief An updated sequence number of your choosing, if Action is Max or Update.
     */
    Azure::Nullable<int64_t> SequenceNumber;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::PageBlobClient::GetPageRanges.
   */
  struct GetPageRangesOptions final
  {
    /**
     * @brief Optionally specifies the range of bytes over which to list ranges, inclusively. If
     * omitted, then all ranges for the blob are returned.
     */
    Azure::Nullable<Core::Http::HttpRange> Range;

    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;

    /**
     * @brief This parameter identifies the portion of the ranges to be returned with the next
     * operation. The operation returns a marker value within the response body if the ranges
     * returned were not complete. The marker value may then be used in a subsequent call to request
     * the next set of ranges.This value is opaque to the client.
     */
    Azure::Nullable<std::string> ContinuationToken;

    /**
     * @brief This parameter specifies the maximum number of page ranges to return. If the request
     * specifies a value greater than 10000, the server will return up to 10000 items. If there are
     * additional results to return, the service returns a continuation token in the NextMarker
     * response element.
     */
    Azure::Nullable<int32_t> PageSizeHint;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::PageBlobClient::StartCopyIncremental.
   */
  struct StartBlobCopyIncrementalOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    BlobAccessConditions AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::SetLegalHold.
   */
  struct SetBlobLegalHoldOptions final
  {
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::SetImmutabilityPolicy.
   */
  struct SetBlobImmutabilityPolicyOptions final
  {
    /**
     * @brief Optional conditions that must be met to perform this operation.
     */
    struct
    {
      /**
       * @brief Specify this header to perform the operation only if the resource has not been
       * modified since the specified time. This timestamp will be truncated to second.
       */
      Azure::Nullable<Azure::DateTime> IfUnmodifiedSince;
    } AccessConditions;
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobClient::DeleteImmutabilityPolicy.
   */
  struct DeleteBlobImmutabilityPolicyOptions final
  {
  };

  /**
   * @brief Optional parameters for #Azure::Storage::Blobs::BlobServiceClient::SubmitBatch.
   */
  struct SubmitBlobBatchOptions final
  {
  };

  namespace _detail {
    inline std::string TagsToString(const std::map<std::string, std::string>& tags)
    {
      return std::accumulate(
          tags.begin(),
          tags.end(),
          std::string(),
          [](const std::string& a, const std::pair<std::string, std::string>& b) {
            return a + (a.empty() ? "" : "&") + _internal::UrlEncodeQueryParameter(b.first) + "="
                + _internal::UrlEncodeQueryParameter(b.second);
          });
    }
  } // namespace _detail
}}} // namespace Azure::Storage::Blobs
