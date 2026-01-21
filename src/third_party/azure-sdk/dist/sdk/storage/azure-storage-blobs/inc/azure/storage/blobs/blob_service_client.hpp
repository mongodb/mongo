// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/blobs/blob_container_client.hpp"

#include <azure/core/credentials/credentials.hpp>
#include <azure/storage/common/storage_credential.hpp>

#include <memory>
#include <string>

namespace Azure { namespace Storage { namespace Blobs {

  class BlobServiceBatch;

  /**
   * The BlobServiceClient allows you to manipulate Azure Storage service resources and blob
   * containers. The storage account provides the top-level namespace for the Blob service.
   */
  class BlobServiceClient final {
  public:
    /**
     * @brief Initialize a new instance of BlobServiceClient.
     *
     * @param connectionString A connection string includes the authentication information required
     * for your application to access data in an Azure Storage account at runtime.
     * @param options Optional client options that define the transport pipeline policies for
     * authentication, retries, etc., that are applied to every request.
     * @return A new BlobServiceClient instance.
     */
    static BlobServiceClient CreateFromConnectionString(
        const std::string& connectionString,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of BlobServiceClient.
     *
     * @param serviceUrl A URL referencing the blob that includes the name of the account.
     * @param credential The shared key credential used to sign requests.
     * @param options Optional client options that define the transport pipeline policies for
     * authentication, retries, etc., that are applied to every request.
     */
    explicit BlobServiceClient(
        const std::string& serviceUrl,
        std::shared_ptr<StorageSharedKeyCredential> credential,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of BlobServiceClient.
     *
     * @param serviceUrl A URL referencing the blob that includes the name of the account.
     * @param credential The token credential used to sign requests.
     * @param options Optional client options that define the transport pipeline policies for
     * authentication, retries, etc., that are applied to every request.
     */
    explicit BlobServiceClient(
        const std::string& serviceUrl,
        std::shared_ptr<Core::Credentials::TokenCredential> credential,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of BlobServiceClient.
     *
     * @param serviceUrl A URL referencing the blob that includes the name of the account, and
     * possibly also a SAS token.
     * @param options Optional client options that define the transport pipeline policies for
     * authentication, retries, etc., that are applied to every request.
     */
    explicit BlobServiceClient(
        const std::string& serviceUrl,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Creates a new BlobContainerClient object with the same URL as this BlobServiceClient.
     * The new BlobContainerClient uses the same request policy pipeline as this BlobServiceClient.
     *
     * @param blobContainerName The name of the container to reference.
     * @return A new BlobContainerClient instance.
     */
    BlobContainerClient GetBlobContainerClient(const std::string& blobContainerName) const;

    /**
     * @brief Gets the blob service's primary URL endpoint.
     *
     * @return the blob service's primary URL endpoint.
     */
    std::string GetUrl() const { return m_serviceUrl.GetAbsoluteUrl(); }

    /**
     * @brief Returns a collection of blob containers in the storage account. Enumerating the
     * blob containers may make multiple requests to the service while fetching all the values.
     * Containers are ordered lexicographically by name.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A ListBlobContainersPagedResponse describing the blob containers in the
     * storage account.
     */
    ListBlobContainersPagedResponse ListBlobContainers(
        const ListBlobContainersOptions& options = ListBlobContainersOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Retrieves a key that can be used to delegate Active Directory authorization to
     * shared access signatures.
     *
     * @param expiresOn Expiration of the key's validity. The time should be specified in UTC, and
     * will be truncated to second.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A deserialized UserDelegationKey instance.
     */
    Azure::Response<Models::UserDelegationKey> GetUserDelegationKey(
        const Azure::DateTime& expiresOn,
        const GetUserDelegationKeyOptions& options = GetUserDelegationKeyOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Sets properties for a storage account's Blob service endpoint, including
     * properties for Storage Analytics, CORS (Cross-Origin Resource Sharing) rules and soft delete
     * settings. You can also use this operation to set the default request version for all incoming
     * requests to the Blob service that do not have a version specified.
     *
     * @param properties The blob service properties.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A SetServicePropertiesResult on successfully setting the properties.
     */
    Azure::Response<Models::SetServicePropertiesResult> SetProperties(
        Models::BlobServiceProperties properties,
        const SetServicePropertiesOptions& options = SetServicePropertiesOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Gets the properties of a storage account's blob service, including properties
     * for Storage Analytics and CORS (Cross-Origin Resource Sharing) rules.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A BlobServiceProperties describing the service properties.
     */
    Azure::Response<Models::BlobServiceProperties> GetProperties(
        const GetServicePropertiesOptions& options = GetServicePropertiesOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Returns the sku name and account kind for the specified account.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return AccountInfo describing the account.
     */
    Azure::Response<Models::AccountInfo> GetAccountInfo(
        const GetAccountInfoOptions& options = GetAccountInfoOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Retrieves statistics related to replication for the Blob service. It is only
     * available on the secondary location endpoint when read-access geo-redundant replication is
     * enabled for the storage account.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A ServiceStatistics describing the service replication statistics.
     */
    Azure::Response<Models::ServiceStatistics> GetStatistics(
        const GetBlobServiceStatisticsOptions& options = GetBlobServiceStatisticsOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief The Filter Blobs operation enables callers to list blobs across all containers whose
     * tags match a given search expression. Filter blobs searches across all containers within a
     * storage account but can be scoped within the expression to a single container.
     *
     * @param tagFilterSqlExpression The where parameter enables the caller to query blobs
     * whose tags match a given expression. The given expression must evaluate to true for a blob to
     * be returned in the results. The[OData - ABNF] filter syntax rule defines the formal grammar
     * for the value of the where query parameter, however, only a subset of the OData filter syntax
     * is supported in the Blob service.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A FindBlobsByTagsPagedResponse describing the blobs.
     */
    FindBlobsByTagsPagedResponse FindBlobsByTags(
        const std::string& tagFilterSqlExpression,
        const FindBlobsByTagsOptions& options = FindBlobsByTagsOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Creates a new blob container under the specified account. If the container with the
     * same name already exists, the operation fails.
     *
     * @param blobContainerName The name of the container to create.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A BlobContainerClient referencing the newly created container.
     */
    Azure::Response<BlobContainerClient> CreateBlobContainer(
        const std::string& blobContainerName,
        const CreateBlobContainerOptions& options = CreateBlobContainerOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Marks the specified blob container for deletion. The container and any blobs
     * contained within it are later deleted during garbage collection.
     *
     * @param blobContainerName The name of the container to delete.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A DeleteBlobContainerResult if successful.
     */
    Azure::Response<Models::DeleteBlobContainerResult> DeleteBlobContainer(
        const std::string& blobContainerName,
        const DeleteBlobContainerOptions& options = DeleteBlobContainerOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Restores a previously deleted container.
     *
     * @param deletedBlobContainerName The name of the previously deleted container.
     * @param deletedBlobContainerVersion The version of the previously deleted container.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A BlobContainerClient referencing the undeleted container.
     */
    Azure::Response<BlobContainerClient> UndeleteBlobContainer(
        const std::string& deletedBlobContainerName,
        const std::string& deletedBlobContainerVersion,
        const UndeleteBlobContainerOptions& options = UndeleteBlobContainerOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Renames an existing blob container.
     * @param sourceBlobContainerName The name of the source container.
     * @param destinationBlobContainerName The name of the destination container.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A BlobContainerClient referencing the renamed container.
     */
    Azure::Response<BlobContainerClient> RenameBlobContainer(
        const std::string& sourceBlobContainerName,
        const std::string& destinationBlobContainerName,
        const RenameBlobContainerOptions& options = RenameBlobContainerOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Creates a new batch object to collect subrequests that can be submitted together via
     * SubmitBatch.
     *
     * @return A new batch object.
     */
    BlobServiceBatch CreateBatch() const;

    /**
     * @brief Submits a batch of subrequests.
     *
     * @param batch The batch object containing subrequests.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A SubmitBlobBatchResult.
     * @remark This function will throw only if there's something wrong with the batch request
     * (parent request).
     */
    Response<Models::SubmitBlobBatchResult> SubmitBatch(
        const BlobServiceBatch& batch,
        const SubmitBlobBatchOptions& options = SubmitBlobBatchOptions(),
        const Core::Context& context = Core::Context()) const;

  private:
    Azure::Core::Url m_serviceUrl;
    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> m_pipeline;
    Azure::Nullable<EncryptionKey> m_customerProvidedKey;
    Azure::Nullable<std::string> m_encryptionScope;

    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> m_batchRequestPipeline;
    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> m_batchSubrequestPipeline;

    friend class BlobServiceBatch;
  };
}}} // namespace Azure::Storage::Blobs
