// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/blobs/blob_responses.hpp"

#include "azure/storage/blobs/blob_client.hpp"
#include "azure/storage/blobs/blob_container_client.hpp"
#include "azure/storage/blobs/blob_service_client.hpp"
#include "azure/storage/blobs/page_blob_client.hpp"

namespace Azure { namespace Storage { namespace Blobs {

  std::unique_ptr<Azure::Core::Http::RawResponse> StartBlobCopyOperation::PollInternal(
      const Azure::Core::Context&)
  {

    auto response = m_blobClient->GetProperties();
    if (!response.Value.CopyStatus.HasValue())
    {
      m_status = Azure::Core::OperationStatus::Failed;
    }
    else if (response.Value.CopyStatus.Value() == Models::CopyStatus::Pending)
    {
      m_status = Azure::Core::OperationStatus::Running;
    }
    else if (response.Value.CopyStatus.Value() == Models::CopyStatus::Success)
    {
      m_status = Azure::Core::OperationStatus::Succeeded;
    }
    else
    {
      m_status = Azure::Core::OperationStatus::Failed;
    }
    m_pollResult = response.Value;
    return std::move(response.RawResponse);
  }

  Azure::Response<Models::BlobProperties> StartBlobCopyOperation::PollUntilDoneInternal(
      std::chrono::milliseconds period,
      Azure::Core::Context& context)
  {
    while (true)
    {
      auto rawResponse = Poll(context);

      if (m_status == Azure::Core::OperationStatus::Succeeded)
      {
        return Azure::Response<Models::BlobProperties>(
            m_pollResult, std::make_unique<Azure::Core::Http::RawResponse>(rawResponse));
      }
      else if (m_status == Azure::Core::OperationStatus::Failed)
      {
        throw Azure::Core::RequestFailedException("Operation failed.");
      }
      else if (m_status == Azure::Core::OperationStatus::Cancelled)
      {
        throw Azure::Core::RequestFailedException("Operation was cancelled.");
      }

      std::this_thread::sleep_for(period);
    }
  }

  void ListBlobContainersPagedResponse::OnNextPage(const Azure::Core::Context& context)
  {
    m_operationOptions.ContinuationToken = NextPageToken;
    *this = m_blobServiceClient->ListBlobContainers(m_operationOptions, context);
  }

  void FindBlobsByTagsPagedResponse::OnNextPage(const Azure::Core::Context& context)
  {
    m_operationOptions.ContinuationToken = NextPageToken;
    if (m_blobServiceClient)
    {
      *this = m_blobServiceClient->FindBlobsByTags(
          m_tagFilterSqlExpression, m_operationOptions, context);
    }
    else if (m_blobContainerClient)
    {
      *this = m_blobContainerClient->FindBlobsByTags(
          m_tagFilterSqlExpression, m_operationOptions, context);
    }
    else
    {
      AZURE_UNREACHABLE_CODE();
    }
  }

  void ListBlobsPagedResponse::OnNextPage(const Azure::Core::Context& context)
  {
    m_operationOptions.ContinuationToken = NextPageToken;
    *this = m_blobContainerClient->ListBlobs(m_operationOptions, context);
  }

  void ListBlobsByHierarchyPagedResponse::OnNextPage(const Azure::Core::Context& context)
  {
    m_operationOptions.ContinuationToken = NextPageToken;
    *this = m_blobContainerClient->ListBlobsByHierarchy(m_delimiter, m_operationOptions, context);
  }

  void GetPageRangesPagedResponse::OnNextPage(const Azure::Core::Context& context)
  {
    m_operationOptions.ContinuationToken = NextPageToken;
    *this = m_pageBlobClient->GetPageRanges(m_operationOptions, context);
  }

  void GetPageRangesDiffPagedResponse::OnNextPage(const Azure::Core::Context& context)
  {
    m_operationOptions.ContinuationToken = NextPageToken;
    if (m_previousSnapshot.HasValue())
    {
      *this = m_pageBlobClient->GetPageRangesDiff(
          m_previousSnapshot.Value(), m_operationOptions, context);
    }
    else if (m_previousSnapshotUrl.HasValue())
    {
      *this = m_pageBlobClient->GetManagedDiskPageRangesDiff(
          m_previousSnapshotUrl.Value(), m_operationOptions, context);
    }
    else
    {
      AZURE_UNREACHABLE_CODE();
    }
  }

}}} // namespace Azure::Storage::Blobs
