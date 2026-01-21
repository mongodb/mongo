// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "azure/storage/blobs/blob_client.hpp"
#include "azure/storage/blobs/blob_container_client.hpp"

#include <chrono>
#include <mutex>
#include <string>

namespace Azure { namespace Storage { namespace Blobs {

  /**
   * @brief BlobLeaseClient allows you to manipulate Azure Storage leases on containers and blobs.
   */
  class BlobLeaseClient final {
  public:
    /**
     * @brief Initializes a new instance of the BlobLeaseClient.
     *
     * @param blobClient A BlobClient representing the blob being leased.
     * @param leaseId A lease ID. This is not required for break operation.
     */
    explicit BlobLeaseClient(BlobClient blobClient, std::string leaseId)
        : m_blobClient(std::move(blobClient)), m_leaseId(std::move(leaseId))
    {
    }

    /**
     * @brief Initializes a new instance of the BlobLeaseClient.
     *
     * @param blobContainerClient A BlobContainerClient representing the blob container being
     * leased.
     * @param leaseId A lease ID. This is not required for break operation.
     */
    explicit BlobLeaseClient(BlobContainerClient blobContainerClient, std::string leaseId)
        : m_blobContainerClient(std::move(blobContainerClient)), m_leaseId(std::move(leaseId))
    {
    }

    /**
     * @brief Gets a unique lease ID.
     *
     * @return A unique lease ID.
     */
    static std::string CreateUniqueLeaseId();

    /**
     * @brief A value representing infinite lease duration.
     */
    AZ_STORAGE_BLOBS_DLLEXPORT const static std::chrono::seconds InfiniteLeaseDuration;

    /**
     * @brief Get lease id of this lease client.
     *
     * @return Lease id of this lease client.
     */
    std::string GetLeaseId()
    {
      std::lock_guard<std::mutex> guard(m_mutex);
      return m_leaseId;
    }

    /**
     * @brief Acquires a lease on the blob or blob container.
     *
     * @param duration Specifies the duration of
     * the lease, in seconds, or InfiniteLeaseDuration for a lease that never
     * expires. A non-infinite lease can be between 15 and 60 seconds. A lease duration cannot be
     * changed using renew or change.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return An AcquireLeaseResult describing the lease.
     */
    Azure::Response<Models::AcquireLeaseResult> Acquire(
        std::chrono::seconds duration,
        const AcquireLeaseOptions& options = AcquireLeaseOptions(),
        const Azure::Core::Context& context = Azure::Core::Context());

    /**
     * @brief Renews the blob or blob container's previously-acquired lease.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A RenewLeaseResult describing the lease.
     */
    Azure::Response<Models::RenewLeaseResult> Renew(
        const RenewLeaseOptions& options = RenewLeaseOptions(),
        const Azure::Core::Context& context = Azure::Core::Context());

    /**
     * @brief Releases the blob or blob container's previously-acquired lease.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A ReleaseLeaseResult describing the updated container or blob.
     */
    Azure::Response<Models::ReleaseLeaseResult> Release(
        const ReleaseLeaseOptions& options = ReleaseLeaseOptions(),
        const Azure::Core::Context& context = Azure::Core::Context());

    /**
     * @brief Changes the lease of an active lease.
     *
     * @param proposedLeaseId Proposed lease ID, in a GUID string format.
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A ChangeLeaseResult describing the changed lease.
     * @remarks The current BlobLeaseClient becomes invalid if this operation succeeds.
     */
    Azure::Response<Models::ChangeLeaseResult> Change(
        const std::string& proposedLeaseId,
        const ChangeLeaseOptions& options = ChangeLeaseOptions(),
        const Azure::Core::Context& context = Azure::Core::Context());

    /**
     * @brief Breaks the previously-acquired lease.
     *
     * @param options Optional parameters to execute this function.
     * @param context Context for cancelling long running operations.
     * @return A BreakLeaseResult describing the broken lease.
     */
    Azure::Response<Models::BreakLeaseResult> Break(
        const BreakLeaseOptions& options = BreakLeaseOptions(),
        const Azure::Core::Context& context = Azure::Core::Context());

  private:
    Azure::Nullable<BlobClient> m_blobClient;
    Azure::Nullable<BlobContainerClient> m_blobContainerClient;
    std::mutex m_mutex;
    std::string m_leaseId;
  };

}}} // namespace Azure::Storage::Blobs
