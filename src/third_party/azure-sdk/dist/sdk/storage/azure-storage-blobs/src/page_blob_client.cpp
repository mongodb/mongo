// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/blobs/page_blob_client.hpp"

#include <azure/storage/common/crypt.hpp>
#include <azure/storage/common/internal/concurrent_transfer.hpp>
#include <azure/storage/common/internal/constants.hpp>
#include <azure/storage/common/internal/file_io.hpp>
#include <azure/storage/common/internal/storage_switch_to_secondary_policy.hpp>
#include <azure/storage/common/storage_common.hpp>
#include <azure/storage/common/storage_exception.hpp>

namespace Azure { namespace Storage { namespace Blobs {

  PageBlobClient PageBlobClient::CreateFromConnectionString(
      const std::string& connectionString,
      const std::string& blobContainerName,
      const std::string& blobName,
      const BlobClientOptions& options)
  {
    PageBlobClient newClient(BlobClient::CreateFromConnectionString(
        connectionString, blobContainerName, blobName, options));
    return newClient;
  }

  PageBlobClient::PageBlobClient(
      const std::string& blobUrl,
      std::shared_ptr<StorageSharedKeyCredential> credential,
      const BlobClientOptions& options)
      : BlobClient(blobUrl, std::move(credential), options)
  {
  }

  PageBlobClient::PageBlobClient(
      const std::string& blobUrl,
      std::shared_ptr<Core::Credentials::TokenCredential> credential,
      const BlobClientOptions& options)
      : BlobClient(blobUrl, std::move(credential), options)
  {
  }

  PageBlobClient::PageBlobClient(const std::string& blobUrl, const BlobClientOptions& options)
      : BlobClient(blobUrl, options)
  {
  }

  PageBlobClient::PageBlobClient(BlobClient blobClient) : BlobClient(std::move(blobClient)) {}

  PageBlobClient PageBlobClient::WithSnapshot(const std::string& snapshot) const
  {
    PageBlobClient newClient(*this);
    if (snapshot.empty())
    {
      newClient.m_blobUrl.RemoveQueryParameter(_internal::HttpQuerySnapshot);
    }
    else
    {
      newClient.m_blobUrl.AppendQueryParameter(
          _internal::HttpQuerySnapshot, _internal::UrlEncodeQueryParameter(snapshot));
    }
    return newClient;
  }

  PageBlobClient PageBlobClient::WithVersionId(const std::string& versionId) const
  {
    PageBlobClient newClient(*this);
    if (versionId.empty())
    {
      newClient.m_blobUrl.RemoveQueryParameter(_internal::HttpQueryVersionId);
    }
    else
    {
      newClient.m_blobUrl.AppendQueryParameter(
          _internal::HttpQueryVersionId, _internal::UrlEncodeQueryParameter(versionId));
    }
    return newClient;
  }

  Azure::Response<Models::CreatePageBlobResult> PageBlobClient::Create(
      int64_t blobSize,
      const CreatePageBlobOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::PageBlobClient::CreatePageBlobOptions protocolLayerOptions;
    protocolLayerOptions.BlobContentLength = blobSize;
    protocolLayerOptions.BlobSequenceNumber = options.SequenceNumber;
    protocolLayerOptions.BlobContentType = options.HttpHeaders.ContentType;
    protocolLayerOptions.BlobContentEncoding = options.HttpHeaders.ContentEncoding;
    protocolLayerOptions.BlobContentLanguage = options.HttpHeaders.ContentLanguage;
    protocolLayerOptions.BlobContentMD5 = options.HttpHeaders.ContentHash.Value;
    protocolLayerOptions.BlobContentDisposition = options.HttpHeaders.ContentDisposition;
    protocolLayerOptions.BlobCacheControl = options.HttpHeaders.CacheControl;
    protocolLayerOptions.Metadata
        = std::map<std::string, std::string>(options.Metadata.begin(), options.Metadata.end());
    protocolLayerOptions.Tier = options.AccessTier;
    protocolLayerOptions.BlobTagsString = _detail::TagsToString(options.Tags);
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    if (m_customerProvidedKey.HasValue())
    {
      protocolLayerOptions.EncryptionKey = m_customerProvidedKey.Value().Key;
      protocolLayerOptions.EncryptionKeySha256 = m_customerProvidedKey.Value().KeyHash;
      protocolLayerOptions.EncryptionAlgorithm = m_customerProvidedKey.Value().Algorithm.ToString();
    }
    protocolLayerOptions.EncryptionScope = m_encryptionScope;
    if (options.ImmutabilityPolicy.HasValue())
    {
      protocolLayerOptions.ImmutabilityPolicyExpiry = options.ImmutabilityPolicy.Value().ExpiresOn;
      protocolLayerOptions.ImmutabilityPolicyMode = options.ImmutabilityPolicy.Value().PolicyMode;
    }
    protocolLayerOptions.LegalHold = options.HasLegalHold;

    return _detail::PageBlobClient::Create(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::CreatePageBlobResult> PageBlobClient::CreateIfNotExists(
      int64_t blobContentLength,
      const CreatePageBlobOptions& options,
      const Azure::Core::Context& context) const
  {
    auto optionsCopy = options;
    optionsCopy.AccessConditions.IfNoneMatch = Azure::ETag::Any();
    try
    {
      return Create(blobContentLength, optionsCopy, context);
    }
    catch (StorageException& e)
    {
      if (e.StatusCode == Core::Http::HttpStatusCode::Conflict
          && e.ErrorCode == "BlobAlreadyExists")
      {
        Models::CreatePageBlobResult ret;
        ret.Created = false;
        return Azure::Response<Models::CreatePageBlobResult>(
            std::move(ret), std::move(e.RawResponse));
      }
      throw;
    }
  }

  Azure::Response<Models::UploadPagesResult> PageBlobClient::UploadPages(
      int64_t offset,
      Azure::Core::IO::BodyStream& content,
      const UploadPagesOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::PageBlobClient::UploadPageBlobPagesOptions protocolLayerOptions;
    protocolLayerOptions.Range
        = "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + content.Length() - 1);
    if (options.TransactionalContentHash.HasValue())
    {
      if (options.TransactionalContentHash.Value().Algorithm == HashAlgorithm::Md5)
      {
        protocolLayerOptions.TransactionalContentMD5
            = options.TransactionalContentHash.Value().Value;
      }
      else if (options.TransactionalContentHash.Value().Algorithm == HashAlgorithm::Crc64)
      {
        protocolLayerOptions.TransactionalContentCrc64
            = options.TransactionalContentHash.Value().Value;
      }
    }
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    protocolLayerOptions.IfSequenceNumberLessThanOrEqualTo
        = options.AccessConditions.IfSequenceNumberLessThanOrEqual;
    protocolLayerOptions.IfSequenceNumberLessThan
        = options.AccessConditions.IfSequenceNumberLessThan;
    protocolLayerOptions.IfSequenceNumberEqualTo = options.AccessConditions.IfSequenceNumberEqual;
    if (m_customerProvidedKey.HasValue())
    {
      protocolLayerOptions.EncryptionKey = m_customerProvidedKey.Value().Key;
      protocolLayerOptions.EncryptionKeySha256 = m_customerProvidedKey.Value().KeyHash;
      protocolLayerOptions.EncryptionAlgorithm = m_customerProvidedKey.Value().Algorithm.ToString();
    }
    protocolLayerOptions.EncryptionScope = m_encryptionScope;
    return _detail::PageBlobClient::UploadPages(
        *m_pipeline, m_blobUrl, content, protocolLayerOptions, context);
  }

  Azure::Response<Models::UploadPagesFromUriResult> PageBlobClient::UploadPagesFromUri(
      int64_t destinationOffset,
      std::string sourceUri,
      Azure::Core::Http::HttpRange sourceRange,
      const UploadPagesFromUriOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::PageBlobClient::UploadPageBlobPagesFromUriOptions protocolLayerOptions;
    protocolLayerOptions.SourceUrl = sourceUri;
    protocolLayerOptions.Range = "bytes=" + std::to_string(destinationOffset) + "-"
        + std::to_string(destinationOffset + sourceRange.Length.Value() - 1);
    protocolLayerOptions.SourceRange = "bytes=" + std::to_string(sourceRange.Offset) + "-"
        + std::to_string(sourceRange.Offset + sourceRange.Length.Value() - 1);
    if (options.TransactionalContentHash.HasValue())
    {
      if (options.TransactionalContentHash.Value().Algorithm == HashAlgorithm::Md5)
      {
        protocolLayerOptions.SourceContentMD5 = options.TransactionalContentHash.Value().Value;
      }
      else if (options.TransactionalContentHash.Value().Algorithm == HashAlgorithm::Crc64)
      {
        protocolLayerOptions.SourceContentcrc64 = options.TransactionalContentHash.Value().Value;
      }
    }
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    protocolLayerOptions.IfSequenceNumberLessThanOrEqualTo
        = options.AccessConditions.IfSequenceNumberLessThanOrEqual;
    protocolLayerOptions.IfSequenceNumberLessThan
        = options.AccessConditions.IfSequenceNumberLessThan;
    protocolLayerOptions.IfSequenceNumberEqualTo = options.AccessConditions.IfSequenceNumberEqual;
    protocolLayerOptions.SourceIfModifiedSince = options.SourceAccessConditions.IfModifiedSince;
    protocolLayerOptions.SourceIfUnmodifiedSince = options.SourceAccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.SourceIfMatch = options.SourceAccessConditions.IfMatch;
    protocolLayerOptions.SourceIfNoneMatch = options.SourceAccessConditions.IfNoneMatch;
    if (m_customerProvidedKey.HasValue())
    {
      protocolLayerOptions.EncryptionKey = m_customerProvidedKey.Value().Key;
      protocolLayerOptions.EncryptionKeySha256 = m_customerProvidedKey.Value().KeyHash;
      protocolLayerOptions.EncryptionAlgorithm = m_customerProvidedKey.Value().Algorithm.ToString();
    }
    protocolLayerOptions.EncryptionScope = m_encryptionScope;
    if (!options.SourceAuthorization.empty())
    {
      protocolLayerOptions.CopySourceAuthorization = options.SourceAuthorization;
    }
    return _detail::PageBlobClient::UploadPagesFromUri(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::ClearPagesResult> PageBlobClient::ClearPages(
      Azure::Core::Http::HttpRange range,
      const ClearPagesOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::PageBlobClient::ClearPageBlobPagesOptions protocolLayerOptions;
    protocolLayerOptions.Range = "bytes=" + std::to_string(range.Offset) + "-"
        + std::to_string(range.Offset + range.Length.Value() - 1);
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    protocolLayerOptions.IfSequenceNumberLessThanOrEqualTo
        = options.AccessConditions.IfSequenceNumberLessThanOrEqual;
    protocolLayerOptions.IfSequenceNumberLessThan
        = options.AccessConditions.IfSequenceNumberLessThan;
    protocolLayerOptions.IfSequenceNumberEqualTo = options.AccessConditions.IfSequenceNumberEqual;
    if (m_customerProvidedKey.HasValue())
    {
      protocolLayerOptions.EncryptionKey = m_customerProvidedKey.Value().Key;
      protocolLayerOptions.EncryptionKeySha256 = m_customerProvidedKey.Value().KeyHash;
      protocolLayerOptions.EncryptionAlgorithm = m_customerProvidedKey.Value().Algorithm.ToString();
    }
    protocolLayerOptions.EncryptionScope = m_encryptionScope;
    return _detail::PageBlobClient::ClearPages(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::ResizePageBlobResult> PageBlobClient::Resize(
      int64_t blobSize,
      const ResizePageBlobOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::PageBlobClient::ResizePageBlobOptions protocolLayerOptions;
    protocolLayerOptions.BlobContentLength = blobSize;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    return _detail::PageBlobClient::Resize(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::UpdateSequenceNumberResult> PageBlobClient::UpdateSequenceNumber(
      Models::SequenceNumberAction action,
      const UpdatePageBlobSequenceNumberOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::PageBlobClient::UpdatePageBlobSequenceNumberOptions protocolLayerOptions;
    protocolLayerOptions.SequenceNumberAction = action;
    protocolLayerOptions.BlobSequenceNumber = options.SequenceNumber;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    return _detail::PageBlobClient::UpdateSequenceNumber(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  GetPageRangesPagedResponse PageBlobClient::GetPageRanges(
      const GetPageRangesOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::PageBlobClient::GetPageBlobPageRangesOptions protocolLayerOptions;
    if (options.Range.HasValue())
    {
      std::string rangeStr = "bytes=" + std::to_string(options.Range.Value().Offset) + "-";
      if (options.Range.Value().Length.HasValue())
      {
        rangeStr += std::to_string(
            options.Range.Value().Offset + options.Range.Value().Length.Value() - 1);
      }
      protocolLayerOptions.Range = rangeStr;
    }
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    protocolLayerOptions.Marker = options.ContinuationToken;
    protocolLayerOptions.MaxResults = options.PageSizeHint;
    auto response = _detail::PageBlobClient::GetPageRanges(
        *m_pipeline, m_blobUrl, protocolLayerOptions, _internal::WithReplicaStatus(context));

    GetPageRangesPagedResponse pagedResponse;

    pagedResponse.ETag = std::move(response.Value.ETag);
    pagedResponse.LastModified = std::move(response.Value.LastModified);
    pagedResponse.BlobSize = response.Value.BlobSize;
    pagedResponse.PageRanges = std::move(response.Value.PageRanges);
    pagedResponse.m_pageBlobClient = std::make_shared<PageBlobClient>(*this);
    pagedResponse.m_operationOptions = options;
    pagedResponse.CurrentPageToken = options.ContinuationToken.ValueOr(std::string());
    pagedResponse.NextPageToken = response.Value.ContinuationToken;
    pagedResponse.RawResponse = std::move(response.RawResponse);

    return pagedResponse;
  }

  GetPageRangesDiffPagedResponse PageBlobClient::GetPageRangesDiff(
      const std::string& previousSnapshot,
      const GetPageRangesOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::PageBlobClient::GetPageBlobPageRangesDiffOptions protocolLayerOptions;
    protocolLayerOptions.Prevsnapshot = previousSnapshot;
    if (options.Range.HasValue())
    {
      std::string rangeStr = "bytes=" + std::to_string(options.Range.Value().Offset) + "-";
      if (options.Range.Value().Length.HasValue())
      {
        rangeStr += std::to_string(
            options.Range.Value().Offset + options.Range.Value().Length.Value() - 1);
      }
      protocolLayerOptions.Range = rangeStr;
    }
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    protocolLayerOptions.Marker = options.ContinuationToken;
    protocolLayerOptions.MaxResults = options.PageSizeHint;
    auto response = _detail::PageBlobClient::GetPageRangesDiff(
        *m_pipeline, m_blobUrl, protocolLayerOptions, _internal::WithReplicaStatus(context));

    GetPageRangesDiffPagedResponse pagedResponse;

    pagedResponse.ETag = std::move(response.Value.ETag);
    pagedResponse.LastModified = std::move(response.Value.LastModified);
    pagedResponse.BlobSize = response.Value.BlobSize;
    pagedResponse.PageRanges = std::move(response.Value.PageRanges);
    pagedResponse.ClearRanges = std::move(response.Value.ClearRanges);
    pagedResponse.m_pageBlobClient = std::make_shared<PageBlobClient>(*this);
    pagedResponse.m_operationOptions = options;
    pagedResponse.m_previousSnapshot = previousSnapshot;
    pagedResponse.CurrentPageToken = options.ContinuationToken.ValueOr(std::string());
    pagedResponse.NextPageToken = response.Value.ContinuationToken;
    pagedResponse.RawResponse = std::move(response.RawResponse);

    return pagedResponse;
  }

  GetPageRangesDiffPagedResponse PageBlobClient::GetManagedDiskPageRangesDiff(
      const std::string& previousSnapshotUrl,
      const GetPageRangesOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::PageBlobClient::GetPageBlobPageRangesDiffOptions protocolLayerOptions;
    protocolLayerOptions.PrevSnapshotUrl = previousSnapshotUrl;
    if (options.Range.HasValue())
    {
      std::string rangeStr = "bytes=" + std::to_string(options.Range.Value().Offset) + "-";
      if (options.Range.Value().Length.HasValue())
      {
        rangeStr += std::to_string(
            options.Range.Value().Offset + options.Range.Value().Length.Value() - 1);
      }
      protocolLayerOptions.Range = rangeStr;
    }
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    protocolLayerOptions.Marker = options.ContinuationToken;
    protocolLayerOptions.MaxResults = options.PageSizeHint;
    auto response = _detail::PageBlobClient::GetPageRangesDiff(
        *m_pipeline, m_blobUrl, protocolLayerOptions, _internal::WithReplicaStatus(context));

    GetPageRangesDiffPagedResponse pagedResponse;

    pagedResponse.ETag = std::move(response.Value.ETag);
    pagedResponse.LastModified = std::move(response.Value.LastModified);
    pagedResponse.BlobSize = response.Value.BlobSize;
    pagedResponse.PageRanges = std::move(response.Value.PageRanges);
    pagedResponse.ClearRanges = std::move(response.Value.ClearRanges);
    pagedResponse.m_pageBlobClient = std::make_shared<PageBlobClient>(*this);
    pagedResponse.m_operationOptions = options;
    pagedResponse.m_previousSnapshotUrl = previousSnapshotUrl;
    pagedResponse.CurrentPageToken = options.ContinuationToken.ValueOr(std::string());
    pagedResponse.NextPageToken = response.Value.ContinuationToken;
    pagedResponse.RawResponse = std::move(response.RawResponse);

    return pagedResponse;
  }

  StartBlobCopyOperation PageBlobClient::StartCopyIncremental(
      const std::string& sourceUri,
      const StartBlobCopyIncrementalOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::PageBlobClient::StartPageBlobCopyIncrementalOptions protocolLayerOptions;
    protocolLayerOptions.CopySource = sourceUri;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;

    auto response = _detail::PageBlobClient::StartCopyIncremental(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
    StartBlobCopyOperation res;
    res.m_rawResponse = std::move(response.RawResponse);
    res.m_blobClient = std::make_shared<BlobClient>(*this);
    return res;
  }

}}} // namespace Azure::Storage::Blobs
