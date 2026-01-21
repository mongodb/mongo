// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/blobs/blob_client.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Azure { namespace Storage { namespace Blobs {

  /**
   * The PageBlobClient allows you to manipulate Azure Storage page blobs.
   *
   * Page blobs are a collection of 512-byte pages optimized for random read and write operations.
   * To create a page blob, you initialize the page blob and specify the maximum size the page blob
   * will grow. To add or update the contents of a page blob, you write a page or pages by
   * specifying an offset and a range that align to 512-byte page boundaries. Writes to page blobs
   * happen in-place and are immediately committed to the blob.
   */
  class PageBlobClient final : public BlobClient {
  public:
    /**
     * @brief Initialize a new instance of PageBlobClient.
     *
     * @param connectionString A connection string includes the authentication information required
     * for your application to access data in an Azure Storage account at runtime.
     * @param blobContainerName The name of the container containing this blob.
     * @param blobName The name of this blob.
     * @param options Optional client options that define the transport pipeline policies for
     * authentication, retries, etc., that are applied to every request.
     * @return A new PageBlobClient instance.
     */
    static PageBlobClient CreateFromConnectionString(
        const std::string& connectionString,
        const std::string& blobContainerName,
        const std::string& blobName,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of PageBlobClient.
     *
     * @param blobUrl A URL
     * referencing the blob that includes the name of the account, the name of the container, and
     * the name of the blob.
     * @param credential The shared key credential used to sign
     * requests.
     * @param options Optional client options that define the transport pipeline
     * policies for authentication, retries, etc., that are applied to every request.
     */
    explicit PageBlobClient(
        const std::string& blobUrl,
        std::shared_ptr<StorageSharedKeyCredential> credential,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of PageBlobClient.
     *
     * @param blobUrl A URL
     * referencing the blob that includes the name of the account, the name of the container, and
     * the name of the blob.
     * @param credential The token credential used to sign requests.
     * @param options Optional client options that define the transport pipeline policies for
     * authentication, retries, etc., that are applied to every request.
     */
    explicit PageBlobClient(
        const std::string& blobUrl,
        std::shared_ptr<Core::Credentials::TokenCredential> credential,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initialize a new instance of PageBlobClient.
     *
     * @param blobUrl A URL
     * referencing the blob that includes the name of the account, the name of the container, and
     * the name of the blob, and possibly also a SAS token.
     * @param options Optional client
     * options that define the transport pipeline policies for authentication, retries, etc., that
     * are applied to every request.
     */
    explicit PageBlobClient(
        const std::string& blobUrl,
        const BlobClientOptions& options = BlobClientOptions());

    /**
     * @brief Initializes a new instance of the PageBlobClient class with an identical URL
     * source but the specified snapshot timestamp.
     *
     * @param snapshot The snapshot
     * identifier.
     * @return A new PageBlobClient instance.
     * @remarks Pass empty string to remove the snapshot returning the base blob.
     */
    PageBlobClient WithSnapshot(const std::string& snapshot) const;

    /**
     * @brief Creates a clone of this instance that references a version ID rather than the base
     * blob.
     *
     * @param versionId The version ID returning a URL to the base blob.
     * @return A new PageBlobClient instance.
     * @remarks Pass empty string to remove the version ID returning the base blob.
     */
    PageBlobClient WithVersionId(const std::string& versionId) const;

    /**
     * @brief Creates a new page blob of the specified size. The content of any existing
     * blob is overwritten with the newly initialized page blob.
     *
     * @param blobSize Specifies the maximum size for the page blob. The size must be
     * aligned to a 512-byte boundary.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A CreatePageBlobResult describing the newly created page blob.
     */
    Azure::Response<Models::CreatePageBlobResult> Create(
        int64_t blobSize,
        const CreatePageBlobOptions& options = CreatePageBlobOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Creates a new page blob of the specified size. The content keeps unchanged if the blob
     * already exists.
     *
     * @param blobContentLength Specifies the maximum size for the page blob. The size must be
     * aligned to a 512-byte boundary.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A CreatePageBlobResult describing the newly created page blob.
     * CreatePageBlobResult.Created is false if the blob already exists.
     */
    Azure::Response<Models::CreatePageBlobResult> CreateIfNotExists(
        int64_t blobContentLength,
        const CreatePageBlobOptions& options = CreatePageBlobOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Writes content to a range of pages in a page blob, starting at offset.
     *
     * @param offset Specifies the starting offset for the content to be written as a page. Given
     * that pages must be aligned with 512-byte boundaries, the start offset must be a modulus of
     * 512.
     * @param content A BodyStream containing the content of the pages to upload.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A UploadPagesResult describing the state of the updated pages.
     */
    Azure::Response<Models::UploadPagesResult> UploadPages(
        int64_t offset,
        Azure::Core::IO::BodyStream& content,
        const UploadPagesOptions& options = UploadPagesOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Writes a range of pages to a page blob where the contents are read from a
     * uri.
     *
     * @param destinationOffset Specifies the starting offset for the content to be written. Given
     * that pages must be aligned with 512-byte boundaries, the start offset must be a modulus of
     * 512.
     * @param sourceUri Specifies the uri of the source blob. The value may be a
     * uri of up to 2 KB in length that specifies a blob. The source blob must either be public or
     * must be authenticated via a shared access signature. If the source blob is public, no
     * authentication is required to perform the operation.
     * @param sourceRange Only upload the bytes of the blob in the sourceUri in the specified range.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A UploadPagesFromUriResult describing the state of the updated pages.
     */
    Azure::Response<Models::UploadPagesFromUriResult> UploadPagesFromUri(
        int64_t destinationOffset,
        std::string sourceUri,
        Azure::Core::Http::HttpRange sourceRange,
        const UploadPagesFromUriOptions& options = UploadPagesFromUriOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Clears one or more pages from the page blob, as specificed by range.
     *
     * @param range Specifies the range of bytes to be cleared. Both the start and end of the range
     * must be specified. For a page clear operation, the page range can be up to the value of the
     * blob's full size. Given that pages must be aligned with 512-byte boundaries, the start and
     * length of the range must be a modulus of 512.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A ClearPagesResult describing the state of the updated pages.
     */
    Azure::Response<Models::ClearPagesResult> ClearPages(
        Azure::Core::Http::HttpRange range,
        const ClearPagesOptions& options = ClearPagesOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Resizes the page blob to the specified size (which must be a multiple of 512). If the
     * specified value is less than the current size of the blob, then all pages above the specified
     * value are cleared.
     *
     * @param blobSize Specifies the maximum size for the page blob. The size must be
     * aligned to a 512-byte boundary.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A ResizePageBlobResult describing the resized page blob.
     */
    Azure::Response<Models::ResizePageBlobResult> Resize(
        int64_t blobSize,
        const ResizePageBlobOptions& options = ResizePageBlobOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Changes the sequence number for this page blob.
     *
     * @param action Specifies how the service should modify the blob's sequence number.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return An UpdateSequenceNumberResult describing the updated page blob.
     */
    Azure::Response<Models::UpdateSequenceNumberResult> UpdateSequenceNumber(
        Models::SequenceNumberAction action,
        const UpdatePageBlobSequenceNumberOptions& options = UpdatePageBlobSequenceNumberOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Returns the list of valid page ranges for a page blob or snapshot of a page blob.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A GetPageRangesResult describing the valid page ranges for this blob.
     */
    GetPageRangesPagedResponse GetPageRanges(
        const GetPageRangesOptions& options = GetPageRangesOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Returns the list of page ranges that differ between a previous snapshot and this page
     * blob. Changes include both updated and cleared pages.
     *
     * @param previousSnapshot Specifies that the response will contain only pages that were changed
     * between target blob and previous snapshot. Changed pages include both updated and cleared
     * pages. The target blob may be a snapshot, as long as the snapshot specified by
     * previousSnapshot is the older of the two.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A GetPageRangesResult describing the valid page ranges for this blob.
     */
    GetPageRangesDiffPagedResponse GetPageRangesDiff(
        const std::string& previousSnapshot,
        const GetPageRangesOptions& options = GetPageRangesOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Returns the list of page ranges that differ between a previous snapshot URL and this
     * page blob. Changes include both updated and cleared pages. This API only works with managed
     * disk storage accounts.
     *
     * @param previousSnapshotUrl This parameter only works with managed disk storage accounts.
     * Specifies that the response will contain only pages that were changed between target blob and
     * previous snapshot. Changed pages include both updated and cleared pages. The target blob may
     * be a snapshot, as long as the snapshot specified by previousSnapshotUrl is the older of the
     * two.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A GetPageRangesResult describing the valid page ranges for this blob.
     */
    GetPageRangesDiffPagedResponse GetManagedDiskPageRangesDiff(
        const std::string& previousSnapshotUrl,
        const GetPageRangesOptions& options = GetPageRangesOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

    /**
     * @brief Starts copying a snapshot of the sourceUri page blob to this page blob. The snapshot
     * is copied such that only the differential changes between the previously copied snapshot
     * are transferred to the destination. The copied snapshots are complete copies of the original
     * snapshot and can be read or copied from as usual.
     *
     * @param sourceUri Specifies the to the source page blob as a uri up to 2 KB in length. The
     * source blob must either be public or must be authenticated via a shared access signature.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A StartBlobCopyOperation describing the state of the copy operation.
     */
    StartBlobCopyOperation StartCopyIncremental(
        const std::string& sourceUri,
        const StartBlobCopyIncrementalOptions& options = StartBlobCopyIncrementalOptions(),
        const Azure::Core::Context& context = Azure::Core::Context()) const;

  private:
    explicit PageBlobClient(BlobClient blobClient);

    friend class BlobClient;
  };

}}} // namespace Azure::Storage::Blobs
