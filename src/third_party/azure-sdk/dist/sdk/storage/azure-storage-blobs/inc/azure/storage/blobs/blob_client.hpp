// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/blobs/blob_options.hpp"
#include "azure/storage/blobs/blob_responses.hpp"
#include "azure/storage/blobs/dll_import_export.hpp"

#include <azure/core/credentials/credentials.hpp>
#include <azure/storage/common/storage_credential.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace Azure { namespace Storage { namespace Files { namespace DataLake {
  class DataLakeFileSystemClient;
  class DataLakeDirectoryClient;
  class DataLakeFileClient;
}}}} // namespace Azure::Storage::Files::DataLake

namespace Azure { namespace Storage { namespace Blobs {

  namespace _detail {
    AZ_STORAGE_BLOBS_DLLEXPORT extern const Azure::Core::Context::Key
        DataLakeInteroperabilityExtraOptionsKey;
  } // namespace _detail

  class BlockBlobClient;
  class AppendBlobClient;
  class PageBlobClient;
  class BlobLeaseClient;

  /**
   * @brief The BlobClient allows you to manipulate Azure Storage blobs.
   */
  class BlobClient {
  public:
    /**
     * @brief Destructor.
     *
     */
    virtual ~BlobClient() = default;

    /**
     * @brief Initialize a new instance of BlobClient.
     *
     * @param connectionString A connection string includes the authentication information required
     * for your application to access data in an Azure Storage account at runtime.
     * @param blobContainerName The name of the container containing this blob.
     * @param blobName The name of this blob.
     * @param options Optional client options that define the transport pipeline policies for
     * authentication, retries, etc., that are applied to every request.
     * @return A new BlobClient instance.
     */
    static BlobClient CreateFromConnectionString(
        const std::string& connectionString,
        const std::string& blobContainerName,
        const std::string& blobName,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of BlobClient.
     *
     * @param blobUrl A URL referencing the blob that includes the name of the account, the name of
     * the container, and the name of the blob.
     * @param credential The shared key credential used to sign requests.
     * @param options Optional client options that define the transport pipeline
     * policies for authentication, retries, etc., that are applied to every request.
     */
    explicit BlobClient(
        const std::string& blobUrl,
        std::shared_ptr<StorageSharedKeyCredential> credential,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of BlobClient.
     *
     * @param blobUrl A URL referencing the blob that includes the name of the account, the name of
     * the container, and the name of the blob.
     * @param credential The token credential used to sign requests.
     * @param options Optional client options that define the transport pipeline policies for
     * authentication, retries, etc., that are applied to every request.
     */
    explicit BlobClient(
        const std::string& blobUrl,
        std::shared_ptr<Core::Credentials::TokenCredential> credential,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of BlobClient.
     *
     * @param blobUrl A URL referencing the blob that includes the name of the account, the name of
     * the container, and the name of the blob, and possibly also a SAS token.
     * @param options Optional client
     * options that define the transport pipeline policies for authentication, retries, etc., that
     * are applied to every request.
     */
    explicit BlobClient(
        const std::string& blobUrl,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Creates a new BlockBlobClient object with the same URL as this BlobClient. The
     * new BlockBlobClient uses the same request policy pipeline as this BlobClient.
     *
     *
     * @return A new BlockBlobClient instance.
     */
    BlockBlobClient AsBlockBlobClient() const;

    /**
     * @brief Creates a new AppendBlobClient object with the same URL as this BlobClient.
     * The new AppendBlobClient uses the same request policy pipeline as this BlobClient.
     *
     * @return A new AppendBlobClient instance.
     */
    AppendBlobClient AsAppendBlobClient() const;

    /**
     * @brief Creates a new PageBlobClient object with the same URL as this BlobClient.
     * The new PageBlobClient uses the same request policy pipeline as this BlobClient.
     *
     * @return A new PageBlobClient instance.
     */
    PageBlobClient AsPageBlobClient() const;

    /**
     * @brief Gets the blob's primary URL endpoint.
     *
     * @return The blob's primary URL endpoint.
     */
    std::string GetUrl() const { return m_blobUrl.GetAbsoluteUrl(); }

    /**
     * @brief Initializes a new instance of the BlobClient class with an identical URL
     * source but the specified snapshot timestamp.
     *
     * @param snapshot The snapshot identifier.
     * @return A new BlobClient instance.
     * @remarks Pass empty string to remove the snapshot returning the base blob.
     */
    BlobClient WithSnapshot(const std::string& snapshot) const;

    /**
     * @brief Creates a clone of this instance that references a version ID rather than the base
     * blob.
     *
     * @param versionId The version ID returning a URL to the base blob.
     * @return A new BlobClient instance.
     * @remarks Pass empty string to remove the version ID returning the base blob.
     */
    BlobClient WithVersionId(const std::string& versionId) const;

    /**
     * @brief Returns all user-defined metadata, standard HTTP properties, and system
     * properties for the blob. It does not return the content of the blob.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A BlobProperties describing the blob's properties.
     */
    Azure::Response<Models::BlobProperties> GetProperties(
        const GetBlobPropertiesOptions& options = GetBlobPropertiesOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Sets system properties on the blob.
     *
     * @param httpHeaders The standard HTTP header system properties to set.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A SetBlobHttpHeadersResult describing the updated blob.
     */
    Azure::Response<Models::SetBlobHttpHeadersResult> SetHttpHeaders(
        Models::BlobHttpHeaders httpHeaders,
        const SetBlobHttpHeadersOptions& options = SetBlobHttpHeadersOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Sets user-defined metadata for the specified blob as one or more name-value
     * pairs.
     *
     * @param metadata Custom metadata to set for this blob.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A SetBlobMetadataResult describing the updated blob.
     */
    Azure::Response<Models::SetBlobMetadataResult> SetMetadata(
        Metadata metadata,
        const SetBlobMetadataOptions& options = SetBlobMetadataOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Sets the tier on a blob. The operation is allowed on a page blob in a premium
     * storage account and on a block blob in a blob storage or general purpose v2 account.
     *
     * @param accessTier Indicates the tier to be set on the blob.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A SetBlobAccessTierResult on successfully setting the tier.
     */
    Azure::Response<Models::SetBlobAccessTierResult> SetAccessTier(
        Models::AccessTier accessTier,
        const SetBlobAccessTierOptions& options = SetBlobAccessTierOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Copies data from the source to this blob, synchronously.
     *
     * @param sourceUri Specifies the URL of the source blob. The value may be a URL of up to 2 KB
     * in length that specifies a blob. The value should be URL-encoded as it would appear in a
     * request URI. The source blob must either be public or must be authorized via a shared access
     * signature. If the size of the source blob is greater than 256 MB, the request will fail with
     * 409 (Conflict). The blob type of the source blob has to be block blob.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A CopyBlobFromUriResult describing the copy result.
     */
    Azure::Response<Models::CopyBlobFromUriResult> CopyFromUri(
        const std::string& sourceUri,
        const CopyBlobFromUriOptions& options = CopyBlobFromUriOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Copies data at from the source to this blob.
     *
     * @param sourceUri
     * Specifies the uri of the source blob. The value may a uri of up to 2 KB in length that
     * specifies a blob. A source blob in the same storage account can be authenticated via Shared
     * Key. However, if the source is a blob in another account, the source blob must either be
     * public or must be authenticated via a shared access signature. If the source blob is public,
     * no authentication is required to perform the copy operation.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A StartBlobCopyOperation describing the state of the copy operation.
     */
    StartBlobCopyOperation StartCopyFromUri(
        const std::string& sourceUri,
        const StartBlobCopyFromUriOptions& options = StartBlobCopyFromUriOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Aborts a pending StartCopyFromUri operation, and leaves this blob with zero
     * length and full metadata.
     *
     * @param copyId ID of the copy operation to abort.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return An AbortBlobCopyFromUriResult on successfully aborting.
     */
    Azure::Response<Models::AbortBlobCopyFromUriResult> AbortCopyFromUri(
        const std::string& copyId,
        const AbortBlobCopyFromUriOptions& options = AbortBlobCopyFromUriOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Downloads a blob or a blob range from the service, including its metadata and
     * properties.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A DownloadBlobResult describing the downloaded blob.
     * BlobDownloadResponse.BodyStream contains the blob's data.
     */
    Azure::Response<Models::DownloadBlobResult> Download(
        const DownloadBlobOptions& options = DownloadBlobOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Downloads a blob or a blob range from the service to a memory buffer using parallel
     * requests.
     *
     * @param buffer A memory buffer to write the blob content to.
     * @param bufferSize Size of the memory buffer. Size must be larger or equal to size of the blob
     * or blob range.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A DownloadBlobToResult describing the downloaded blob.
     */
    Azure::Response<Models::DownloadBlobToResult> DownloadTo(
        uint8_t* buffer,
        size_t bufferSize,
        const DownloadBlobToOptions& options = DownloadBlobToOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Downloads a blob or a blob range from the service to a file using parallel
     * requests.
     *
     * @param fileName A file path to write the downloaded content to.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A DownloadBlobToResult describing the downloaded blob.
     */
    Azure::Response<Models::DownloadBlobToResult> DownloadTo(
        const std::string& fileName,
        const DownloadBlobToOptions& options = DownloadBlobToOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Creates a read-only snapshot of a blob.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A CreateBlobSnapshotResult describing the new blob snapshot.
     */
    Azure::Response<Models::CreateBlobSnapshotResult> CreateSnapshot(
        const CreateBlobSnapshotOptions& options = CreateBlobSnapshotOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Marks the specified blob or snapshot for deletion. The blob is later deleted
     * during garbage collection. Note that in order to delete a blob, you must delete all of its
     * snapshots. You can delete both at the same time using DeleteBlobOptions.DeleteSnapshots.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A DeleteBlobResult on successfully deleting.
     */
    Azure::Response<Models::DeleteBlobResult> Delete(
        const DeleteBlobOptions& options = DeleteBlobOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Marks the specified blob or snapshot for deletion if it exists.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A DeleteBlobResult on successfully deleting. DeleteBlobResult.Deleted is false if the
     * blob doesn't exist.
     */
    Azure::Response<Models::DeleteBlobResult> DeleteIfExists(
        const DeleteBlobOptions& options = DeleteBlobOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Restores the contents and metadata of a soft deleted blob and any associated
     * soft deleted snapshots.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A UndeleteBlobResult on successfully deleting.
     */
    Azure::Response<Models::UndeleteBlobResult> Undelete(
        const UndeleteBlobOptions& options = UndeleteBlobOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Sets tags on the underlying blob.
     *
     * @param tags The tags to set on the blob.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A SetBlobTagsInfo on successfully setting tags.
     */
    Azure::Response<Models::SetBlobTagsResult> SetTags(
        std::map<std::string, std::string> tags,
        const SetBlobTagsOptions& options = SetBlobTagsOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Gets the tags associated with the underlying blob.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return BlobTags on successfully getting tags.
     */
    Azure::Response<std::map<std::string, std::string>> GetTags(
        const GetBlobTagsOptions& options = GetBlobTagsOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Sets the immutability policy on a blob, snapshot or version. Note that Blob Versioning
     * must be enabled on your storage account, and the blob must be in a Container with immutable
     * storage with versioning enabled to call this API.
     *
     * @param immutabilityPolicy The blob immutability policy to set.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return The blob's immutability policy.
     */
    Azure::Response<Models::SetBlobImmutabilityPolicyResult> SetImmutabilityPolicy(
        Models::BlobImmutabilityPolicy immutabilityPolicy,
        const SetBlobImmutabilityPolicyOptions& options = SetBlobImmutabilityPolicyOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Deletes the Immutability Policy associated with the Blob.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A DeleteBlobImmutabilityPolicyResult on successfully deleting immutability policy.
     */
    Azure::Response<Models::DeleteBlobImmutabilityPolicyResult> DeleteImmutabilityPolicy(
        const DeleteBlobImmutabilityPolicyOptions& options = DeleteBlobImmutabilityPolicyOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Sets a legal hold on the blob. Note that Blob Versioning must be enabled on your
     * storage account, and the blob must be in a Container with immutable storage with versioning
     * enabled to call this API.
     *
     * @param hasLegalHold Set to true to set a legal hold on the blob. Set to false to remove an
     * existing legal hold.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A SetBlobLegalHoldResult on successfully setting legal hold.
     */
    Azure::Response<Models::SetBlobLegalHoldResult> SetLegalHold(
        bool hasLegalHold,
        const SetBlobLegalHoldOptions& options = SetBlobLegalHoldOptions(),
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

  protected:
    /** @brief Blob Url */
    Azure::Core::Url m_blobUrl;
    /** @brief Http Pipeline */
    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> m_pipeline;
    /** @brief Customer provided encryption key. */
    Azure::Nullable<EncryptionKey> m_customerProvidedKey;
    /** @brief Encryption scope. */
    Azure::Nullable<std::string> m_encryptionScope;

  private:
    explicit BlobClient(
        Azure::Core::Url blobUrl,
        std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> pipeline,
        Azure::Nullable<EncryptionKey> customerProvidedKey = Azure::Nullable<EncryptionKey>(),
        Azure::Nullable<std::string> encryptionScope = Azure::Nullable<std::string>())
        : m_blobUrl(std::move(blobUrl)), m_pipeline(std::move(pipeline)),
          m_customerProvidedKey(std::move(customerProvidedKey)),
          m_encryptionScope(std::move(encryptionScope))
    {
    }

    friend class BlobContainerClient;
    friend class Files::DataLake::DataLakeFileSystemClient;
    friend class Files::DataLake::DataLakeDirectoryClient;
    friend class Files::DataLake::DataLakeFileClient;
    friend class BlobLeaseClient;
    friend class BlobServiceBatch;
    friend class BlobContainerBatch;
  };
}}} // namespace Azure::Storage::Blobs
