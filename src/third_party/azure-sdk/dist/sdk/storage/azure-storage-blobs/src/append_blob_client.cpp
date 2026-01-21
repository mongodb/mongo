// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/blobs/append_blob_client.hpp"

#include <azure/storage/common/crypt.hpp>
#include <azure/storage/common/internal/constants.hpp>
#include <azure/storage/common/storage_common.hpp>
#include <azure/storage/common/storage_exception.hpp>

namespace Azure { namespace Storage { namespace Blobs {

  AppendBlobClient AppendBlobClient::CreateFromConnectionString(
      const std::string& connectionString,
      const std::string& blobContainerName,
      const std::string& blobName,
      const BlobClientOptions& options)
  {
    AppendBlobClient newClient(BlobClient::CreateFromConnectionString(
        connectionString, blobContainerName, blobName, options));
    return newClient;
  }

  AppendBlobClient::AppendBlobClient(
      const std::string& blobUrl,
      std::shared_ptr<StorageSharedKeyCredential> credential,
      const BlobClientOptions& options)
      : BlobClient(blobUrl, std::move(credential), options)
  {
  }

  AppendBlobClient::AppendBlobClient(
      const std::string& blobUrl,
      std::shared_ptr<Core::Credentials::TokenCredential> credential,
      const BlobClientOptions& options)
      : BlobClient(blobUrl, std::move(credential), options)
  {
  }

  AppendBlobClient::AppendBlobClient(const std::string& blobUrl, const BlobClientOptions& options)
      : BlobClient(blobUrl, options)
  {
  }

  AppendBlobClient::AppendBlobClient(BlobClient blobClient) : BlobClient(std::move(blobClient)) {}

  AppendBlobClient AppendBlobClient::WithSnapshot(const std::string& snapshot) const
  {
    AppendBlobClient newClient(*this);
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

  AppendBlobClient AppendBlobClient::WithVersionId(const std::string& versionId) const
  {
    AppendBlobClient newClient(*this);
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

  Azure::Response<Models::CreateAppendBlobResult> AppendBlobClient::Create(
      const CreateAppendBlobOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::AppendBlobClient::CreateAppendBlobOptions protocolLayerOptions;
    protocolLayerOptions.BlobContentType = options.HttpHeaders.ContentType;
    protocolLayerOptions.BlobContentEncoding = options.HttpHeaders.ContentEncoding;
    protocolLayerOptions.BlobContentLanguage = options.HttpHeaders.ContentLanguage;
    protocolLayerOptions.BlobContentMD5 = options.HttpHeaders.ContentHash.Value;
    protocolLayerOptions.BlobContentDisposition = options.HttpHeaders.ContentDisposition;
    protocolLayerOptions.BlobCacheControl = options.HttpHeaders.CacheControl;
    protocolLayerOptions.Metadata
        = std::map<std::string, std::string>(options.Metadata.begin(), options.Metadata.end());
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

    return _detail::AppendBlobClient::Create(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::CreateAppendBlobResult> AppendBlobClient::CreateIfNotExists(
      const CreateAppendBlobOptions& options,
      const Azure::Core::Context& context) const
  {
    auto optionsCopy = options;
    optionsCopy.AccessConditions.IfNoneMatch = Azure::ETag::Any();
    try
    {
      return Create(optionsCopy, context);
    }
    catch (StorageException& e)
    {
      if (e.StatusCode == Core::Http::HttpStatusCode::Conflict
          && e.ErrorCode == "BlobAlreadyExists")
      {
        Models::CreateAppendBlobResult ret;
        ret.Created = false;
        return Azure::Response<Models::CreateAppendBlobResult>(
            std::move(ret), std::move(e.RawResponse));
      }
      throw;
    }
  }

  Azure::Response<Models::AppendBlockResult> AppendBlobClient::AppendBlock(
      Azure::Core::IO::BodyStream& content,
      const AppendBlockOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::AppendBlobClient::AppendAppendBlobBlockOptions protocolLayerOptions;
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
    protocolLayerOptions.MaxSize = options.AccessConditions.IfMaxSizeLessThanOrEqual;
    protocolLayerOptions.AppendPosition = options.AccessConditions.IfAppendPositionEqual;
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
    return _detail::AppendBlobClient::AppendBlock(
        *m_pipeline, m_blobUrl, content, protocolLayerOptions, context);
  }

  Azure::Response<Models::AppendBlockFromUriResult> AppendBlobClient::AppendBlockFromUri(
      const std::string& sourceUri,
      const AppendBlockFromUriOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::AppendBlobClient::AppendAppendBlobBlockFromUriOptions protocolLayerOptions;
    protocolLayerOptions.SourceUrl = sourceUri;
    if (options.SourceRange.HasValue())
    {
      std::string rangeStr = "bytes=" + std::to_string(options.SourceRange.Value().Offset) + "-";
      if (options.SourceRange.Value().Length.HasValue())
      {
        rangeStr += std::to_string(
            options.SourceRange.Value().Offset + options.SourceRange.Value().Length.Value() - 1);
      }
      protocolLayerOptions.SourceRange = rangeStr;
    }
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
    protocolLayerOptions.MaxSize = options.AccessConditions.IfMaxSizeLessThanOrEqual;
    protocolLayerOptions.AppendPosition = options.AccessConditions.IfAppendPositionEqual;
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
    if (!options.SourceAuthorization.empty())
    {
      protocolLayerOptions.CopySourceAuthorization = options.SourceAuthorization;
    }
    return _detail::AppendBlobClient::AppendBlockFromUri(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::SealAppendBlobResult> AppendBlobClient::Seal(
      const SealAppendBlobOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::AppendBlobClient::SealAppendBlobOptions protocolLayerOptions;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.AppendPosition = options.AccessConditions.IfAppendPositionEqual;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    return _detail::AppendBlobClient::Seal(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

}}} // namespace Azure::Storage::Blobs
