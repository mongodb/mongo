// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/blobs/blob_client.hpp"
#include "azure/storage/blobs/blob_container_client.hpp"
#include "azure/storage/blobs/blob_service_client.hpp"
#include "azure/storage/blobs/deferred_response.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Azure { namespace Storage { namespace Blobs {

  namespace _detail {
    extern const Core::Context::Key s_serviceBatchKey;
    extern const Core::Context::Key s_containerBatchKey;

    class StringBodyStream final : public Core::IO::BodyStream {
    public:
      explicit StringBodyStream(std::string content) : m_content(std::move(content)) {}
      StringBodyStream(const StringBodyStream&) = delete;
      StringBodyStream& operator=(const StringBodyStream&) = delete;
      StringBodyStream(StringBodyStream&& other) = default;
      StringBodyStream& operator=(StringBodyStream&& other) = default;
      ~StringBodyStream() override {}
      int64_t Length() const override { return m_content.length(); }
      void Rewind() override { m_offset = 0; }

    private:
      size_t OnRead(uint8_t* buffer, size_t count, Azure::Core::Context const& context) override;

    private:
      std::string m_content;
      size_t m_offset = 0;
    };

    enum class BatchSubrequestType
    {
      DeleteBlob,
      SetBlobAccessTier,
    };

    struct BatchSubrequest
    {
      explicit BatchSubrequest(BatchSubrequestType type) : Type(type) {}
      virtual ~BatchSubrequest() = 0;

      BatchSubrequestType Type;
    };

    class BlobBatchAccessHelper;

    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> ConstructBatchRequestPolicy(
        const std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>&
            servicePerRetryPolicies,
        const std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>&
            servicePerOperationPolicies,
        const BlobClientOptions& options);

    std::shared_ptr<Azure::Core::Http::_internal::HttpPipeline> ConstructBatchSubrequestPolicy(
        std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>&& tokenAuthPolicy,
        std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>&& sharedKeyAuthPolicy,
        const BlobClientOptions& options);
  } // namespace _detail

  /**
   * @brief A batch object allows you to batch multiple operations in a single request via
   * #Azure::Storage::Blobs::BlobServiceClient::SubmitBatch.
   */
  class BlobServiceBatch final {
  public:
    /**
     * @brief Adds a delete subrequest into batch object.
     *
     * @param blobContainerName Container name of the blob to delete.
     * @param blobName Name of the blob to delete.
     * @param options Optional parameters to execute the delete operation.
     * @return A deferred response which can produce a Response<DeleteBlobResult> after batch object
     * is submitted.
     */
    DeferredResponse<Models::DeleteBlobResult> DeleteBlob(
        const std::string& blobContainerName,
        const std::string& blobName,
        const DeleteBlobOptions& options = DeleteBlobOptions());

    /**
     * @brief Adds a delete subrequest into batch object.
     *
     * @param blobUrl Url of the blob to delete.
     * @param options Optional parameters to execute the delete operation.
     * @return A deferred response which can produce a Response<DeleteBlobResult> after batch object
     * is submitted.
     */
    DeferredResponse<Models::DeleteBlobResult> DeleteBlobUrl(
        const std::string& blobUrl,
        const DeleteBlobOptions& options = DeleteBlobOptions());

    /**
     * @brief Adds a change tier subrequest into batch object.
     *
     * @param blobContainerName Container name of the blob to delete.
     * @param blobName Name of the blob to delete.
     * @param accessTier Indicates the tier to be set on the blob.
     * @param options Optional parameters to execute the delete operation.
     * @return A deferred response which can produce a Response<SetBlobAccessTierResult> after batch
     * object is submitted.
     */
    DeferredResponse<Models::SetBlobAccessTierResult> SetBlobAccessTier(
        const std::string& blobContainerName,
        const std::string& blobName,
        Models::AccessTier accessTier,
        const SetBlobAccessTierOptions& options = SetBlobAccessTierOptions());

    /**
     * @brief Adds a change tier subrequest into batch object.
     *
     * @param blobUrl Url of the blob to delete.
     * @param accessTier Indicates the tier to be set on the blob.
     * @param options Optional parameters to execute the delete operation.
     * @return A deferred response which can produce a Response<SetBlobAccessTierResult> after batch
     * object is submitted.
     */
    DeferredResponse<Models::SetBlobAccessTierResult> SetBlobAccessTierUrl(
        const std::string& blobUrl,
        Models::AccessTier accessTier,
        const SetBlobAccessTierOptions& options = SetBlobAccessTierOptions());

  private:
    explicit BlobServiceBatch(BlobServiceClient blobServiceClient);

    BlobClient GetBlobClientForSubrequest(Core::Url url) const;

  private:
    BlobServiceClient m_blobServiceClient;

    std::vector<std::shared_ptr<_detail::BatchSubrequest>> m_subrequests;

    friend class BlobServiceClient;
    friend class _detail::BlobBatchAccessHelper;
  };

  /**
   * @brief A batch object allows you to batch multiple operations in a single request via
   * #Azure::Storage::Blobs::BlobContainerClient::SubmitBatch.
   */
  class BlobContainerBatch final {
  public:
    /**
     * @brief Adds a delete subrequest into batch object.
     *
     * @param blobName Name of the blob to delete.
     * @param options Optional parameters to execute the delete operation.
     * @return A deferred response which can produce a Response<DeleteBlobResult> after batch object
     * is submitted.
     */
    DeferredResponse<Models::DeleteBlobResult> DeleteBlob(
        const std::string& blobName,
        const DeleteBlobOptions& options = DeleteBlobOptions());

    /**
     * @brief Adds a delete subrequest into batch object.
     *
     * @param blobUrl Url of the blob to delete.
     * @param options Optional parameters to execute the delete operation.
     * @return A deferred response which can produce a Response<DeleteBlobResult> after batch object
     * is submitted.
     */
    DeferredResponse<Models::DeleteBlobResult> DeleteBlobUrl(
        const std::string& blobUrl,
        const DeleteBlobOptions& options = DeleteBlobOptions());

    /**
     * @brief Adds a change tier subrequest into batch object.
     *
     * @param blobName Name of the blob to delete.
     * @param accessTier Indicates the tier to be set on the blob.
     * @param options Optional parameters to execute the delete operation.
     * @return A deferred response which can produce a Response<SetBlobAccessTierResult> after batch
     * object is submitted.
     */
    DeferredResponse<Models::SetBlobAccessTierResult> SetBlobAccessTier(
        const std::string& blobName,
        Models::AccessTier accessTier,
        const SetBlobAccessTierOptions& options = SetBlobAccessTierOptions());

    /**
     * @brief Adds a change tier subrequest into batch object.
     *
     * @param blobUrl Url of the blob to delete.
     * @param accessTier Indicates the tier to be set on the blob.
     * @param options Optional parameters to execute the delete operation.
     * @return A deferred response which can produce a Response<SetBlobAccessTierResult> after batch
     * object is submitted.
     */
    DeferredResponse<Models::SetBlobAccessTierResult> SetBlobAccessTierUrl(
        const std::string& blobUrl,
        Models::AccessTier accessTier,
        const SetBlobAccessTierOptions& options = SetBlobAccessTierOptions());

  private:
    explicit BlobContainerBatch(BlobContainerClient blobContainerClient);

    BlobClient GetBlobClientForSubrequest(Core::Url url) const;

  private:
    BlobContainerClient m_blobContainerClient;

    std::vector<std::shared_ptr<_detail::BatchSubrequest>> m_subrequests;

    friend class BlobContainerClient;
    friend class _detail::BlobBatchAccessHelper;
  };

}}} // namespace Azure::Storage::Blobs
