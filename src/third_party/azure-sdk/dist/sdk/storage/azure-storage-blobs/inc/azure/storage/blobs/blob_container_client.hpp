// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/blobs/blob_client.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Azure { namespace Storage { namespace Blobs {

  class BlobLeaseClient;
  class BlobContainerBatch;

  /**
   * The BlobContainerClient allows you to manipulate Azure Storage containers and their
   * blobs.
   */
  class BlobContainerClient final {
  public:
    /**
     * @brief Initialize a new instance of BlobContainerClient.
     *
     * @param connectionString A connection string includes the authentication information required
     * for your application to access data in an Azure Storage account at runtime.
     * @param blobContainerName The name of the container containing this blob.
     * @param options Optional client options that define the transport pipeline policies for
     * authentication, retries, etc., that are applied to every request.
     * @return A new BlobContainerClient instance.
     */
    static BlobContainerClient CreateFromConnectionString(
        const std::string& connectionString,
        const std::string& blobContainerName,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of BlobContainerClient.
     *
     * @param blobContainerUrl A url
     * referencing the blob container that includes the name of the account and the name of the
     * container.
     * @param credential The shared key credential used to sign
     * requests.
     * @param options Optional client options that define the transport pipeline
     * policies for authentication, retries, etc., that are applied to every request.
     */
    explicit BlobContainerClient(
        const std::string& blobContainerUrl,
        std::shared_ptr<StorageSharedKeyCredential> credential,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of BlobContainerClient.
     *
     * @param blobContainerUrl A url
     * referencing the blob container that includes the name of the account and the name of the
     * container.
     * @param credential The token credential used to sign requests.
     * @param options Optional client options that define the transport pipeline policies for
     * authentication, retries, etc., that are applied to every request.
     */
    explicit BlobContainerClient(
        const std::string& blobContainerUrl,
        std::shared_ptr<Core::Credentials::TokenCredential> credential,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of BlobContainerClient.
     *
     * @param blobContainerUrl A url
     * referencing the blob that includes the name of the account and the name of the container, and
     * possibly also a SAS token.
     * @param options Optional client
     * options that define the transport pipeline policies for authentication, retries, etc., that
     * are applied to every request.
     */
    explicit BlobContainerClient(
        const std::string& blobContainerUrl,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Create a new BlobClient object by appending blobName to the end of URL. The
     * new BlobClient uses the same request policy pipeline as this BlobContainerClient.
     *
     * @param blobName The name of the blob.
     * @return A new BlobClient instance.
     */
    BlobClient GetBlobClient(const std::string& blobName) const;

    /**
     * @brief Create a new BlockBlobClient object by appending blobName to the end of URL.
     * The new BlockBlobClient uses the same request policy pipeline as this BlobContainerClient.
     *
     * @param blobName The name of the blob.
     * @return A new BlockBlobClient instance.
     */
    BlockBlobClient GetBlockBlobClient(const std::string& blobName) const;

    /**
     * @brief Create a new AppendBlobClient object by appending blobName to the end of URL.
     * The new AppendBlobClient uses the same request policy pipeline as this BlobContainerClient.
     *
     * @param blobName The name of the blob.
     * @return A new AppendBlobClient instance.
     */
    AppendBlobClient GetAppendBlobClient(const std::string& blobName) const;

    /**
     * @brief Create a new PageBlobClient object by appending blobName to the end of URL.
     * The new PageBlobClient uses the same request policy pipeline as this BlobContainerClient.
     *
     * @param blobName The name of the blob.
     * @return A new PageBlobClient instance.
     */
    PageBlobClient GetPageBlobClient(const std::string& blobName) const;

    /**
     * @brief Gets the container's primary URL endpoint.
     *
     * @return The container's primary URL endpoint.
     */
    std::string GetUrl() const { return m_blobContainerUrl.GetAbsoluteUrl(); }

    /**
     * @brief Creates a new container under the specified account. If the container with the
     * same name already exists, the operation fails.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A CreateBlobContainerResult describing the newly created blob container.
     */
    Azure::Response<Models::CreateBlobContainerResult> Create(
        const CreateBlobContainerOptions& options = CreateBlobContainerOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Creates a new container under the specified account. If the container with the
     * same name already exists, it is not changed.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A CreateBlobContainerResult describing the newly created blob container if the
     * container doesn't exist. CreateBlobContainerResult.Created is false if the container already
     * exists.
     */
    Azure::Response<Models::CreateBlobContainerResult> CreateIfNotExists(
        const CreateBlobContainerOptions& options = CreateBlobContainerOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Marks the specified container for deletion. The container and any blobs
     * contained within it are later deleted during garbage collection.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A DeleteBlobContainerResult if successful.
     */
    Azure::Response<Models::DeleteBlobContainerResult> Delete(
        const DeleteBlobContainerOptions& options = DeleteBlobContainerOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Marks the specified container for deletion if it exists. The container and any blobs
     * contained within it are later deleted during garbage collection.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A DeleteBlobContainerResult if the container exists.
     * DeleteBlobContainerResult.Deleted is false if the container doesn't exist.
     */
    Azure::Response<Models::DeleteBlobContainerResult> DeleteIfExists(
        const DeleteBlobContainerOptions& options = DeleteBlobContainerOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Returns all user-defined metadata and system properties for the specified
     * container. The data returned does not include the container's list of blobs.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A BlobContainerProperties describing the container and its properties.
     */
    Azure::Response<Models::BlobContainerProperties> GetProperties(
        const GetBlobContainerPropertiesOptions& options = GetBlobContainerPropertiesOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Sets one or more user-defined name-value pairs for the specified container.
     *
     * @param metadata Custom metadata to set for this container.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A SetBlobContainerMetadataResult if successful.
     */
    Azure::Response<Models::SetBlobContainerMetadataResult> SetMetadata(
        Metadata metadata,
        SetBlobContainerMetadataOptions options = SetBlobContainerMetadataOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Returns a sequence of blobs in this container. Enumerating the blobs may make
     * multiple requests to the service while fetching all the values. Blobs are ordered
     * lexicographically by name.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A ListBlobsPagedResponse describing the blobs in the container.
     */
    ListBlobsPagedResponse ListBlobs(
        const ListBlobsOptions& options = ListBlobsOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Returns a collection of blobs in this container. Enumerating the blobs may make
     * multiple requests to the service while fetching all the values. Blobs are ordered
     * lexicographically by name. A delimiter can be used to traverse a virtual hierarchy of blobs
     * as though it were a file system.
     *
     * @param delimiter This can be used to to traverse a virtual hierarchy of blobs as though it
     * were a file system. The delimiter may be a single character or a string.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A ListBlobsByHierarchyPagedResponse describing the blobs in the container.
     */
    ListBlobsByHierarchyPagedResponse ListBlobsByHierarchy(
        const std::string& delimiter,
        const ListBlobsOptions& options = ListBlobsOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Gets the permissions for this container. The permissions indicate whether
     * container data may be accessed publicly.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A BlobContainerAccessPolicy describing the container's access policy.
     */
    Azure::Response<Models::BlobContainerAccessPolicy> GetAccessPolicy(
        const GetBlobContainerAccessPolicyOptions& options = GetBlobContainerAccessPolicyOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Sets the permissions for the specified container. The permissions indicate
     * whether blob container data may be accessed publicly.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A SetBlobContainerAccessPolicyResult describing the updated container.
     */
    Azure::Response<Models::SetBlobContainerAccessPolicyResult> SetAccessPolicy(
        const SetBlobContainerAccessPolicyOptions& options = SetBlobContainerAccessPolicyOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Marks the specified blob or snapshot for deletion. The blob is later deleted
     * during garbage collection. Note that in order to delete a blob, you must delete all of its
     * snapshots. You can delete both at the same time using DeleteBlobOptions.DeleteSnapshots.
     *
     * @param blobName The name of the blob to delete.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A DeleteBlobResult on successfully deleting.
     */
    Azure::Response<Models::DeleteBlobResult> DeleteBlob(
        const std::string& blobName,
        const DeleteBlobOptions& options = DeleteBlobOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Creates a new block blob under this container. For partial block blob updates and
     * other advanced features, please see BlockBlobClient. To create or modify page or see
     * PageBlobClient or AppendBlobClient.
     *
     * @param blobName The name of the blob to create.
     * @param content A BodyStream containing the content to upload.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A BlockBlobClient referencing the newly created block blob.
     */
    Azure::Response<BlockBlobClient> UploadBlob(
        const std::string& blobName,
        Azure::Core::IO::BodyStream& content,
        const UploadBlockBlobOptions& options = UploadBlockBlobOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief The Filter Blobs operation enables callers to list blobs in a container whose
     * tags match a given search expression.
     *
     * @param tagFilterSqlExpression The where parameter enables the caller to query blobs
     * whose tags match a given expression. The given expression must evaluate to true for a blob to
     * be returned in the results. The [OData - ABNF] filter syntax rule defines the formal grammar
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
     * @brief Creates a new batch object to collect subrequests that can be submitted together via
     * SubmitBatch.
     *
     * @return A new batch object.
     */
    BlobContainerBatch CreateBatch() const;

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
        const BlobContainerBatch& batch,
        const SubmitBlobBatchOptions& options = SubmitBlobBatchOptions(),
        const Core::Context& context = Core::Context()) const;

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

  private:
    Azure::Core::Url m_blobContainerUrl;
    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> m_pipeline;
    Azure::Nullable<EncryptionKey> m_customerProvidedKey;
    Azure::Nullable<std::string> m_encryptionScope;

    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> m_batchRequestPipeline;
    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> m_batchSubrequestPipeline;

    friend class BlobServiceClient;
    friend class BlobLeaseClient;
    friend class BlobContainerBatch;
    friend class Files::DataLake::DataLakeFileSystemClient;
  };

}}} // namespace Azure::Storage::Blobs
