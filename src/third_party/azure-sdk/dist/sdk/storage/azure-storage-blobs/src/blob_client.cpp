// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/storage/blobs/blob_client.hpp"

#include "azure/storage/blobs/append_blob_client.hpp"
#include "azure/storage/blobs/block_blob_client.hpp"
#include "azure/storage/blobs/page_blob_client.hpp"
#include "private/package_version.hpp"

#include <azure/core/azure_assert.hpp>
#include <azure/core/http/policies/policy.hpp>
#include <azure/storage/common/crypt.hpp>
#include <azure/storage/common/internal/concurrent_transfer.hpp>
#include <azure/storage/common/internal/constants.hpp>
#include <azure/storage/common/internal/file_io.hpp>
#include <azure/storage/common/internal/reliable_stream.hpp>
#include <azure/storage/common/internal/shared_key_policy.hpp>
#include <azure/storage/common/internal/storage_bearer_token_auth.hpp>
#include <azure/storage/common/internal/storage_per_retry_policy.hpp>
#include <azure/storage/common/internal/storage_service_version_policy.hpp>
#include <azure/storage/common/internal/storage_switch_to_secondary_policy.hpp>
#include <azure/storage/common/storage_common.hpp>
#include <azure/storage/common/storage_exception.hpp>

#include <algorithm>

namespace Azure { namespace Storage { namespace Blobs {

  namespace _detail {
    Azure::Core::Context::Key const DataLakeInteroperabilityExtraOptionsKey;
  }

  BlobClient BlobClient::CreateFromConnectionString(
      const std::string& connectionString,
      const std::string& blobContainerName,
      const std::string& blobName,
      const BlobClientOptions& options)
  {
    auto parsedConnectionString = _internal::ParseConnectionString(connectionString);
    auto blobUrl = std::move(parsedConnectionString.BlobServiceUrl);
    blobUrl.AppendPath(_internal::UrlEncodePath(blobContainerName));
    blobUrl.AppendPath(_internal::UrlEncodePath(blobName));

    if (parsedConnectionString.KeyCredential)
    {
      return BlobClient(blobUrl.GetAbsoluteUrl(), parsedConnectionString.KeyCredential, options);
    }
    else
    {
      return BlobClient(blobUrl.GetAbsoluteUrl(), options);
    }
  }

  BlobClient::BlobClient(
      const std::string& blobUrl,
      std::shared_ptr<StorageSharedKeyCredential> credential,
      const BlobClientOptions& options)
      : BlobClient(blobUrl, options)
  {
    BlobClientOptions newOptions = options;
    newOptions.PerRetryPolicies.emplace_back(
        std::make_unique<_internal::SharedKeyPolicy>(credential));

    std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> perRetryPolicies;
    std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> perOperationPolicies;
    perRetryPolicies.emplace_back(std::make_unique<_internal::StorageSwitchToSecondaryPolicy>(
        m_blobUrl.GetHost(), newOptions.SecondaryHostForRetryReads));
    perRetryPolicies.emplace_back(std::make_unique<_internal::StoragePerRetryPolicy>());
    perOperationPolicies.emplace_back(
        std::make_unique<_internal::StorageServiceVersionPolicy>(newOptions.ApiVersion));
    m_pipeline = std::make_shared<Azure::Core::Http::_internal::HttpPipeline>(
        newOptions,
        _internal::BlobServicePackageName,
        _detail::PackageVersion::ToString(),
        std::move(perRetryPolicies),
        std::move(perOperationPolicies));
  }

  BlobClient::BlobClient(
      const std::string& blobUrl,
      std::shared_ptr<Core::Credentials::TokenCredential> credential,
      const BlobClientOptions& options)
      : BlobClient(blobUrl, options)
  {
    std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> perRetryPolicies;
    std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> perOperationPolicies;
    perRetryPolicies.emplace_back(std::make_unique<_internal::StorageSwitchToSecondaryPolicy>(
        m_blobUrl.GetHost(), options.SecondaryHostForRetryReads));
    perRetryPolicies.emplace_back(std::make_unique<_internal::StoragePerRetryPolicy>());
    {
      Azure::Core::Credentials::TokenRequestContext tokenContext;
      tokenContext.Scopes.emplace_back(
          options.Audience.HasValue()
              ? _internal::GetDefaultScopeForAudience(options.Audience.Value().ToString())
              : _internal::StorageScope);
      perRetryPolicies.emplace_back(
          std::make_unique<_internal::StorageBearerTokenAuthenticationPolicy>(
              credential, tokenContext, options.EnableTenantDiscovery));
    }
    perOperationPolicies.emplace_back(
        std::make_unique<_internal::StorageServiceVersionPolicy>(options.ApiVersion));
    m_pipeline = std::make_shared<Azure::Core::Http::_internal::HttpPipeline>(
        options,
        _internal::BlobServicePackageName,
        _detail::PackageVersion::ToString(),
        std::move(perRetryPolicies),
        std::move(perOperationPolicies));
  }

  BlobClient::BlobClient(const std::string& blobUrl, const BlobClientOptions& options)
      : m_blobUrl(blobUrl), m_customerProvidedKey(options.CustomerProvidedKey),
        m_encryptionScope(options.EncryptionScope)
  {
    std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> perRetryPolicies;
    std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> perOperationPolicies;
    perRetryPolicies.emplace_back(std::make_unique<_internal::StorageSwitchToSecondaryPolicy>(
        m_blobUrl.GetHost(), options.SecondaryHostForRetryReads));
    perRetryPolicies.emplace_back(std::make_unique<_internal::StoragePerRetryPolicy>());
    perOperationPolicies.emplace_back(
        std::make_unique<_internal::StorageServiceVersionPolicy>(options.ApiVersion));
    m_pipeline = std::make_shared<Azure::Core::Http::_internal::HttpPipeline>(
        options,
        _internal::BlobServicePackageName,
        _detail::PackageVersion::ToString(),
        std::move(perRetryPolicies),
        std::move(perOperationPolicies));
  }

  BlockBlobClient BlobClient::AsBlockBlobClient() const { return BlockBlobClient(*this); }

  AppendBlobClient BlobClient::AsAppendBlobClient() const { return AppendBlobClient(*this); }

  PageBlobClient BlobClient::AsPageBlobClient() const { return PageBlobClient(*this); }

  BlobClient BlobClient::WithSnapshot(const std::string& snapshot) const
  {
    BlobClient newClient(*this);
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

  BlobClient BlobClient::WithVersionId(const std::string& versionId) const
  {
    BlobClient newClient(*this);
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

  Azure::Response<Models::DownloadBlobResult> BlobClient::Download(
      const DownloadBlobOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::DownloadBlobOptions protocolLayerOptions;
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
    if (options.RangeHashAlgorithm.HasValue())
    {
      if (options.RangeHashAlgorithm.Value() == HashAlgorithm::Md5)
      {
        protocolLayerOptions.RangeGetContentMD5 = true;
      }
      else if (options.RangeHashAlgorithm.Value() == HashAlgorithm::Crc64)
      {
        protocolLayerOptions.RangeGetContentCRC64 = true;
      }
    }
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    {
      bool includeUserPrincipalName = false;
      if (context.TryGetValue(
              _detail::DataLakeInteroperabilityExtraOptionsKey, includeUserPrincipalName))
      {
        protocolLayerOptions.UserPrincipalName = includeUserPrincipalName;
      }
    }
    if (m_customerProvidedKey.HasValue())
    {
      protocolLayerOptions.EncryptionKey = m_customerProvidedKey.Value().Key;
      protocolLayerOptions.EncryptionKeySha256 = m_customerProvidedKey.Value().KeyHash;
      protocolLayerOptions.EncryptionAlgorithm = m_customerProvidedKey.Value().Algorithm.ToString();
    }

    auto downloadResponse = _detail::BlobClient::Download(
        *m_pipeline, m_blobUrl, protocolLayerOptions, _internal::WithReplicaStatus(context));

    {
      // In case network failure during reading the body
      const Azure::ETag eTag = downloadResponse.Value.Details.ETag;
      const std::string client_request_id
          = downloadResponse.RawResponse->GetHeaders().find(_internal::HttpHeaderClientRequestId)
              == downloadResponse.RawResponse->GetHeaders().end()
          ? std::string()
          : downloadResponse.RawResponse->GetHeaders().at(_internal::HttpHeaderClientRequestId);
      auto retryFunction =
          [this, options, eTag, client_request_id](
              int64_t retryOffset,
              const Azure::Core::Context& context) -> std::unique_ptr<Azure::Core::IO::BodyStream> {
        DownloadBlobOptions newOptions = options;
        newOptions.Range = Core::Http::HttpRange();
        newOptions.Range.Value().Offset
            = (options.Range.HasValue() ? options.Range.Value().Offset : 0) + retryOffset;
        if (options.Range.HasValue() && options.Range.Value().Length.HasValue())
        {
          newOptions.Range.Value().Length = options.Range.Value().Length.Value() - retryOffset;
        }
        newOptions.AccessConditions.IfMatch = eTag;
        return std::move(
            Download(
                newOptions,
                context.WithValue(_internal::ReliableStreamClientRequestIdKey, client_request_id))
                .Value.BodyStream);
      };

      _internal::ReliableStreamOptions reliableStreamOptions;
      reliableStreamOptions.MaxRetryRequests = _internal::ReliableStreamRetryCount;
      downloadResponse.Value.BodyStream = std::make_unique<_internal::ReliableStream>(
          std::move(downloadResponse.Value.BodyStream), reliableStreamOptions, retryFunction);
    }
    if (downloadResponse.RawResponse->GetStatusCode() == Azure::Core::Http::HttpStatusCode::Ok)
    {
      downloadResponse.Value.BlobSize = std::stoll(
          downloadResponse.RawResponse->GetHeaders().at(_internal::HttpHeaderContentLength));
      downloadResponse.Value.ContentRange.Offset = 0;
      downloadResponse.Value.ContentRange.Length = downloadResponse.Value.BlobSize;
    }
    else if (
        downloadResponse.RawResponse->GetStatusCode()
        == Azure::Core::Http::HttpStatusCode::PartialContent)
    {
      const std::string& contentRange
          = downloadResponse.RawResponse->GetHeaders().at(_internal::HttpHeaderContentRange);
      auto bytes_pos = contentRange.find("bytes ");
      auto dash_pos = contentRange.find("-", bytes_pos + 6);
      auto slash_pos = contentRange.find("/", dash_pos + 1);
      const int64_t rangeStartOffset = std::stoll(
          std::string(contentRange.begin() + bytes_pos + 6, contentRange.begin() + dash_pos));
      const int64_t rangeEndOffset = std::stoll(
          std::string(contentRange.begin() + dash_pos + 1, contentRange.begin() + slash_pos));
      downloadResponse.Value.ContentRange
          = Azure::Core::Http::HttpRange{rangeStartOffset, rangeEndOffset - rangeStartOffset + 1};
      downloadResponse.Value.BlobSize = std::stoll(contentRange.substr(slash_pos + 1));
    }
    if (downloadResponse.Value.BlobType == Models::BlobType::AppendBlob
        && !downloadResponse.Value.Details.IsSealed.HasValue())
    {
      downloadResponse.Value.Details.IsSealed = false;
    }
    if (downloadResponse.Value.Details.VersionId.HasValue()
        && !downloadResponse.Value.Details.IsCurrentVersion.HasValue())
    {
      downloadResponse.Value.Details.IsCurrentVersion = false;
    }
    {
      std::map<std::string, std::vector<Models::ObjectReplicationRule>> orPropertiesMap;
      for (auto i = downloadResponse.RawResponse->GetHeaders().lower_bound("x-ms-or-");
           i != downloadResponse.RawResponse->GetHeaders().end()
           && i->first.substr(0, 8) == "x-ms-or-";
           ++i)
      {
        const std::string& header = i->first;
        auto underscorePos = header.find('_', 8);
        if (underscorePos == std::string::npos)
        {
          continue;
        }
        std::string policyId = std::string(header.begin() + 8, header.begin() + underscorePos);
        std::string ruleId = header.substr(underscorePos + 1);

        Models::ObjectReplicationRule rule;
        rule.RuleId = std::move(ruleId);
        rule.ReplicationStatus = Models::ObjectReplicationStatus(i->second);
        orPropertiesMap[policyId].emplace_back(std::move(rule));
      }
      for (auto& property : orPropertiesMap)
      {
        Models::ObjectReplicationPolicy policy;
        policy.PolicyId = property.first;
        policy.Rules = std::move(property.second);
        downloadResponse.Value.Details.ObjectReplicationSourceProperties.emplace_back(
            std::move(policy));
      }
    }
    return downloadResponse;
  }

  Azure::Response<Models::DownloadBlobToResult> BlobClient::DownloadTo(
      uint8_t* buffer,
      size_t bufferSize,
      const DownloadBlobToOptions& options,
      const Azure::Core::Context& context) const
  {
    // Just start downloading using an initial chunk. If it's a small blob, we'll get the whole
    // thing in one shot. If it's a large blob, we'll get its full size in Content-Range and can
    // keep downloading it in chunks.
    const int64_t firstChunkOffset = options.Range.HasValue() ? options.Range.Value().Offset : 0;
    int64_t firstChunkLength = options.TransferOptions.InitialChunkSize;
    if (options.Range.HasValue() && options.Range.Value().Length.HasValue())
    {
      firstChunkLength = (std::min)(firstChunkLength, options.Range.Value().Length.Value());
    }

    DownloadBlobOptions firstChunkOptions;
    firstChunkOptions.Range = options.Range;
    if (firstChunkOptions.Range.HasValue())
    {
      firstChunkOptions.Range.Value().Length = firstChunkLength;
    }

    auto firstChunk = Download(firstChunkOptions, context);
    const Azure::ETag eTag = firstChunk.Value.Details.ETag;

    const int64_t blobSize = firstChunk.Value.BlobSize;
    int64_t blobRangeSize;
    if (firstChunkOptions.Range.HasValue())
    {
      blobRangeSize = blobSize - firstChunkOffset;
      if (options.Range.HasValue() && options.Range.Value().Length.HasValue())
      {
        blobRangeSize = (std::min)(blobRangeSize, options.Range.Value().Length.Value());
      }
    }
    else
    {
      blobRangeSize = blobSize;
    }
    firstChunkLength = (std::min)(firstChunkLength, blobRangeSize);

    if (static_cast<uint64_t>(blobRangeSize) > (std::numeric_limits<size_t>::max)()
        || static_cast<size_t>(blobRangeSize) > bufferSize)
    {
      throw Azure::Core::RequestFailedException(
          "Buffer is not big enough, blob range size is " + std::to_string(blobRangeSize) + ".");
    }

    int64_t bytesRead = firstChunk.Value.BodyStream->ReadToCount(
        buffer, static_cast<size_t>(firstChunkLength), context);
    if (bytesRead != firstChunkLength)
    {
      throw Azure::Core::RequestFailedException("Error when reading body stream.");
    }
    firstChunk.Value.BodyStream.reset();

    auto returnTypeConverter = [](Azure::Response<Models::DownloadBlobResult>& response) {
      Models::DownloadBlobToResult ret;
      ret.BlobType = std::move(response.Value.BlobType);
      ret.ContentRange = std::move(response.Value.ContentRange);
      ret.BlobSize = response.Value.BlobSize;
      ret.TransactionalContentHash = std::move(response.Value.TransactionalContentHash);
      ret.Details = std::move(response.Value.Details);
      return Azure::Response<Models::DownloadBlobToResult>(
          std::move(ret), std::move(response.RawResponse));
    };
    auto ret = returnTypeConverter(firstChunk);

    // Keep downloading the remaining in parallel
    auto downloadChunkFunc
        = [&](int64_t offset, int64_t length, int64_t chunkId, int64_t numChunks) {
            DownloadBlobOptions chunkOptions;
            chunkOptions.Range = Core::Http::HttpRange();
            chunkOptions.Range.Value().Offset = offset;
            chunkOptions.Range.Value().Length = length;
            chunkOptions.AccessConditions.IfMatch = eTag;
            auto chunk = Download(chunkOptions, context);
            int64_t bytesRead = chunk.Value.BodyStream->ReadToCount(
                buffer + (offset - firstChunkOffset),
                static_cast<size_t>(chunkOptions.Range.Value().Length.Value()),
                context);
            if (bytesRead != chunkOptions.Range.Value().Length.Value())
            {
              throw Azure::Core::RequestFailedException("Error when reading body stream.");
            }

            if (chunkId == numChunks - 1)
            {
              ret = returnTypeConverter(chunk);
              ret.Value.TransactionalContentHash.Reset();
            }
          };

    int64_t remainingOffset = firstChunkOffset + firstChunkLength;
    int64_t remainingSize = blobRangeSize - firstChunkLength;

    _internal::ConcurrentTransfer(
        remainingOffset,
        remainingSize,
        options.TransferOptions.ChunkSize,
        options.TransferOptions.Concurrency,
        downloadChunkFunc);
    ret.Value.ContentRange.Offset = firstChunkOffset;
    ret.Value.ContentRange.Length = blobRangeSize;
    return ret;
  }

  Azure::Response<Models::DownloadBlobToResult> BlobClient::DownloadTo(
      const std::string& fileName,
      const DownloadBlobToOptions& options,
      const Azure::Core::Context& context) const
  {
    // Just start downloading using an initial chunk. If it's a small blob, we'll get the whole
    // thing in one shot. If it's a large blob, we'll get its full size in Content-Range and can
    // keep downloading it in chunks.
    const int64_t firstChunkOffset = options.Range.HasValue() ? options.Range.Value().Offset : 0;
    int64_t firstChunkLength = options.TransferOptions.InitialChunkSize;
    if (options.Range.HasValue() && options.Range.Value().Length.HasValue())
    {
      firstChunkLength = (std::min)(firstChunkLength, options.Range.Value().Length.Value());
    }

    DownloadBlobOptions firstChunkOptions;
    firstChunkOptions.Range = options.Range;
    if (firstChunkOptions.Range.HasValue())
    {
      firstChunkOptions.Range.Value().Length = firstChunkLength;
    }

    auto firstChunk = Download(firstChunkOptions, context);
    const Azure::ETag eTag = firstChunk.Value.Details.ETag;

    const int64_t blobSize = firstChunk.Value.BlobSize;
    int64_t blobRangeSize;
    if (firstChunkOptions.Range.HasValue())
    {
      blobRangeSize = blobSize - firstChunkOffset;
      if (options.Range.HasValue() && options.Range.Value().Length.HasValue())
      {
        blobRangeSize = (std::min)(blobRangeSize, options.Range.Value().Length.Value());
      }
    }
    else
    {
      blobRangeSize = blobSize;
    }
    firstChunkLength = (std::min)(firstChunkLength, blobRangeSize);

    auto bodyStreamToFile = [](Azure::Core::IO::BodyStream& stream,
                               _internal::FileWriter& fileWriter,
                               int64_t offset,
                               int64_t length,
                               const Azure::Core::Context& context) {
      constexpr size_t bufferSize = 4 * 1024 * 1024;
      std::vector<uint8_t> buffer(bufferSize);
      while (length > 0)
      {
        size_t readSize = static_cast<size_t>(std::min<int64_t>(bufferSize, length));
        size_t bytesRead = stream.ReadToCount(buffer.data(), readSize, context);
        if (bytesRead != readSize)
        {
          throw Azure::Core::RequestFailedException("Error when reading body stream.");
        }
        fileWriter.Write(buffer.data(), bytesRead, offset);
        length -= bytesRead;
        offset += bytesRead;
      }
    };

    _internal::FileWriter fileWriter(fileName);
    bodyStreamToFile(*(firstChunk.Value.BodyStream), fileWriter, 0, firstChunkLength, context);
    firstChunk.Value.BodyStream.reset();

    auto returnTypeConverter = [](Azure::Response<Models::DownloadBlobResult>& response) {
      Models::DownloadBlobToResult ret;
      ret.BlobType = std::move(response.Value.BlobType);
      ret.ContentRange = std::move(response.Value.ContentRange);
      ret.BlobSize = response.Value.BlobSize;
      ret.TransactionalContentHash = std::move(response.Value.TransactionalContentHash);
      ret.Details = std::move(response.Value.Details);
      return Azure::Response<Models::DownloadBlobToResult>(
          std::move(ret), std::move(response.RawResponse));
    };
    auto ret = returnTypeConverter(firstChunk);

    // Keep downloading the remaining in parallel
    auto downloadChunkFunc
        = [&](int64_t offset, int64_t length, int64_t chunkId, int64_t numChunks) {
            DownloadBlobOptions chunkOptions;
            chunkOptions.Range = Core::Http::HttpRange();
            chunkOptions.Range.Value().Offset = offset;
            chunkOptions.Range.Value().Length = length;
            chunkOptions.AccessConditions.IfMatch = eTag;
            auto chunk = Download(chunkOptions, context);
            bodyStreamToFile(
                *(chunk.Value.BodyStream),
                fileWriter,
                offset - firstChunkOffset,
                chunkOptions.Range.Value().Length.Value(),
                context);

            if (chunkId == numChunks - 1)
            {
              ret = returnTypeConverter(chunk);
              ret.Value.TransactionalContentHash.Reset();
            }
          };

    int64_t remainingOffset = firstChunkOffset + firstChunkLength;
    int64_t remainingSize = blobRangeSize - firstChunkLength;

    _internal::ConcurrentTransfer(
        remainingOffset,
        remainingSize,
        options.TransferOptions.ChunkSize,
        options.TransferOptions.Concurrency,
        downloadChunkFunc);
    ret.Value.ContentRange.Offset = firstChunkOffset;
    ret.Value.ContentRange.Length = blobRangeSize;
    return ret;
  }

  Azure::Response<Models::BlobProperties> BlobClient::GetProperties(
      const GetBlobPropertiesOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::GetBlobPropertiesOptions protocolLayerOptions;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    {
      bool includeUserPrincipalName = false;
      if (context.TryGetValue(
              _detail::DataLakeInteroperabilityExtraOptionsKey, includeUserPrincipalName))
      {
        protocolLayerOptions.UserPrincipalName = includeUserPrincipalName;
      }
    }
    if (m_customerProvidedKey.HasValue())
    {
      protocolLayerOptions.EncryptionKey = m_customerProvidedKey.Value().Key;
      protocolLayerOptions.EncryptionKeySha256 = m_customerProvidedKey.Value().KeyHash;
      protocolLayerOptions.EncryptionAlgorithm = m_customerProvidedKey.Value().Algorithm.ToString();
    }
    auto response = _detail::BlobClient::GetProperties(
        *m_pipeline, m_blobUrl, protocolLayerOptions, _internal::WithReplicaStatus(context));
    if (response.Value.AccessTier.HasValue() && !response.Value.IsAccessTierInferred.HasValue())
    {
      response.Value.IsAccessTierInferred = false;
    }
    if (response.Value.VersionId.HasValue() && !response.Value.IsCurrentVersion.HasValue())
    {
      response.Value.IsCurrentVersion = false;
    }
    if (response.Value.CopyStatus.HasValue() && !response.Value.IsIncrementalCopy.HasValue())
    {
      response.Value.IsIncrementalCopy = false;
    }
    if (response.Value.BlobType == Models::BlobType::AppendBlob
        && !response.Value.IsSealed.HasValue())
    {
      response.Value.IsSealed = false;
    }
    {
      std::map<std::string, std::vector<Models::ObjectReplicationRule>> orPropertiesMap;
      for (auto i = response.RawResponse->GetHeaders().lower_bound("x-ms-or-");
           i != response.RawResponse->GetHeaders().end() && i->first.substr(0, 8) == "x-ms-or-";
           ++i)
      {
        const std::string& header = i->first;
        auto underscorePos = header.find('_', 8);
        if (underscorePos == std::string::npos)
        {
          continue;
        }
        std::string policyId = std::string(header.begin() + 8, header.begin() + underscorePos);
        std::string ruleId = header.substr(underscorePos + 1);

        Models::ObjectReplicationRule rule;
        rule.RuleId = std::move(ruleId);
        rule.ReplicationStatus = Models::ObjectReplicationStatus(i->second);
        orPropertiesMap[policyId].emplace_back(std::move(rule));
      }
      for (auto& property : orPropertiesMap)
      {
        Models::ObjectReplicationPolicy policy;
        policy.PolicyId = property.first;
        policy.Rules = std::move(property.second);
        response.Value.ObjectReplicationSourceProperties.emplace_back(std::move(policy));
      }
    }
    return response;
  }

  Azure::Response<Models::SetBlobHttpHeadersResult> BlobClient::SetHttpHeaders(
      Models::BlobHttpHeaders httpHeaders,
      const SetBlobHttpHeadersOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::SetBlobHttpHeadersOptions protocolLayerOptions;
    protocolLayerOptions.BlobContentType = std::move(httpHeaders.ContentType);
    protocolLayerOptions.BlobContentEncoding = std::move(httpHeaders.ContentEncoding);
    protocolLayerOptions.BlobContentLanguage = std::move(httpHeaders.ContentLanguage);
    protocolLayerOptions.BlobContentMD5 = std::move(httpHeaders.ContentHash.Value);
    protocolLayerOptions.BlobContentDisposition = std::move(httpHeaders.ContentDisposition);
    protocolLayerOptions.BlobCacheControl = std::move(httpHeaders.CacheControl);
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    return _detail::BlobClient::SetHttpHeaders(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::SetBlobMetadataResult> BlobClient::SetMetadata(
      Metadata metadata,
      const SetBlobMetadataOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::SetBlobMetadataOptions protocolLayerOptions;
    protocolLayerOptions.Metadata
        = std::map<std::string, std::string>(metadata.begin(), metadata.end());
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
    return _detail::BlobClient::SetMetadata(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::SetBlobAccessTierResult> BlobClient::SetAccessTier(
      Models::AccessTier tier,
      const SetBlobAccessTierOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::SetBlobTierOptions protocolLayerOptions;
    protocolLayerOptions.Tier = tier;
    protocolLayerOptions.RehydratePriority = options.RehydratePriority;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    return _detail::BlobClient::SetTier(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::CopyBlobFromUriResult> BlobClient::CopyFromUri(
      const std::string& sourceUri,
      const CopyBlobFromUriOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::CopyBlobFromUriOptions protocolLayerOptions;
    protocolLayerOptions.Metadata
        = std::map<std::string, std::string>(options.Metadata.begin(), options.Metadata.end());
    protocolLayerOptions.BlobTagsString = _detail::TagsToString(options.Tags);
    protocolLayerOptions.CopySource = sourceUri;
    protocolLayerOptions.Tier = options.AccessTier;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    protocolLayerOptions.SourceIfModifiedSince = options.SourceAccessConditions.IfModifiedSince;
    protocolLayerOptions.SourceIfUnmodifiedSince = options.SourceAccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.SourceIfMatch = options.SourceAccessConditions.IfMatch;
    protocolLayerOptions.SourceIfNoneMatch = options.SourceAccessConditions.IfNoneMatch;
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
    if (options.ImmutabilityPolicy.HasValue())
    {
      protocolLayerOptions.ImmutabilityPolicyExpiry = options.ImmutabilityPolicy.Value().ExpiresOn;
      protocolLayerOptions.ImmutabilityPolicyMode = options.ImmutabilityPolicy.Value().PolicyMode;
    }
    protocolLayerOptions.LegalHold = options.HasLegalHold;
    protocolLayerOptions.EncryptionScope = m_encryptionScope;
    protocolLayerOptions.CopySourceTags = options.CopySourceTagsMode;
    if (!options.SourceAuthorization.empty())
    {
      protocolLayerOptions.CopySourceAuthorization = options.SourceAuthorization;
    }

    return _detail::BlobClient::CopyFromUri(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  StartBlobCopyOperation BlobClient::StartCopyFromUri(
      const std::string& sourceUri,
      const StartBlobCopyFromUriOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::StartBlobCopyFromUriOptions protocolLayerOptions;
    protocolLayerOptions.Metadata
        = std::map<std::string, std::string>(options.Metadata.begin(), options.Metadata.end());
    protocolLayerOptions.BlobTagsString = _detail::TagsToString(options.Tags);
    protocolLayerOptions.CopySource = sourceUri;
    protocolLayerOptions.Tier = options.AccessTier;
    protocolLayerOptions.RehydratePriority = options.RehydratePriority;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    protocolLayerOptions.SourceLeaseId = options.SourceAccessConditions.LeaseId;
    protocolLayerOptions.SourceIfModifiedSince = options.SourceAccessConditions.IfModifiedSince;
    protocolLayerOptions.SourceIfUnmodifiedSince = options.SourceAccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.SourceIfMatch = options.SourceAccessConditions.IfMatch;
    protocolLayerOptions.SourceIfNoneMatch = options.SourceAccessConditions.IfNoneMatch;
    protocolLayerOptions.SealBlob = options.ShouldSealDestination;
    protocolLayerOptions.SourceIfTags = options.SourceAccessConditions.TagConditions;
    if (options.ImmutabilityPolicy.HasValue())
    {
      protocolLayerOptions.ImmutabilityPolicyExpiry = options.ImmutabilityPolicy.Value().ExpiresOn;
      protocolLayerOptions.ImmutabilityPolicyMode = options.ImmutabilityPolicy.Value().PolicyMode;
    }
    protocolLayerOptions.LegalHold = options.HasLegalHold;

    auto response = _detail::BlobClient::StartCopyFromUri(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
    StartBlobCopyOperation res;
    res.m_rawResponse = std::move(response.RawResponse);
    res.m_blobClient = std::make_shared<BlobClient>(*this);
    return res;
  }

  Azure::Response<Models::AbortBlobCopyFromUriResult> BlobClient::AbortCopyFromUri(
      const std::string& copyId,
      const AbortBlobCopyFromUriOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::AbortBlobCopyFromUriOptions protocolLayerOptions;
    protocolLayerOptions.CopyId = copyId;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    return _detail::BlobClient::AbortCopyFromUri(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::CreateBlobSnapshotResult> BlobClient::CreateSnapshot(
      const CreateBlobSnapshotOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::CreateBlobSnapshotOptions protocolLayerOptions;
    protocolLayerOptions.Metadata
        = std::map<std::string, std::string>(options.Metadata.begin(), options.Metadata.end());
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
    return _detail::BlobClient::CreateSnapshot(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::DeleteBlobResult> BlobClient::Delete(
      const DeleteBlobOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::DeleteBlobOptions protocolLayerOptions;
    protocolLayerOptions.DeleteSnapshots = options.DeleteSnapshots;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    protocolLayerOptions.IfModifiedSince = options.AccessConditions.IfModifiedSince;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    protocolLayerOptions.IfMatch = options.AccessConditions.IfMatch;
    protocolLayerOptions.IfNoneMatch = options.AccessConditions.IfNoneMatch;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    return _detail::BlobClient::Delete(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::DeleteBlobResult> BlobClient::DeleteIfExists(
      const DeleteBlobOptions& options,
      const Azure::Core::Context& context) const
  {
    try
    {
      return Delete(options, context);
    }
    catch (StorageException& e)
    {
      if (e.StatusCode == Core::Http::HttpStatusCode::NotFound
          && (e.ErrorCode == "BlobNotFound" || e.ErrorCode == "ContainerNotFound"))
      {
        Models::DeleteBlobResult ret;
        ret.Deleted = false;
        return Azure::Response<Models::DeleteBlobResult>(std::move(ret), std::move(e.RawResponse));
      }
      throw;
    }
  }

  Azure::Response<Models::UndeleteBlobResult> BlobClient::Undelete(
      const UndeleteBlobOptions& options,
      const Azure::Core::Context& context) const
  {
    (void)options;
    _detail::BlobClient::UndeleteBlobOptions protocolLayerOptions;
    return _detail::BlobClient::Undelete(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::SetBlobTagsResult> BlobClient::SetTags(
      std::map<std::string, std::string> tags,
      const SetBlobTagsOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::SetBlobTagsOptions protocolLayerOptions;
    protocolLayerOptions.Tags = std::move(tags);
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    return _detail::BlobClient::SetTags(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<std::map<std::string, std::string>> BlobClient::GetTags(
      const GetBlobTagsOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::GetBlobTagsOptions protocolLayerOptions;
    protocolLayerOptions.IfTags = options.AccessConditions.TagConditions;
    protocolLayerOptions.LeaseId = options.AccessConditions.LeaseId;
    return _detail::BlobClient::GetTags(
        *m_pipeline, m_blobUrl, protocolLayerOptions, _internal::WithReplicaStatus(context));
  }

  Azure::Response<Models::SetBlobImmutabilityPolicyResult> BlobClient::SetImmutabilityPolicy(
      Models::BlobImmutabilityPolicy immutabilityPolicy,
      const SetBlobImmutabilityPolicyOptions& options,
      const Azure::Core::Context& context) const
  {
    _detail::BlobClient::SetBlobImmutabilityPolicyOptions protocolLayerOptions;
    protocolLayerOptions.ImmutabilityPolicyExpiry = immutabilityPolicy.ExpiresOn;
    protocolLayerOptions.ImmutabilityPolicyMode = immutabilityPolicy.PolicyMode;
    protocolLayerOptions.IfUnmodifiedSince = options.AccessConditions.IfUnmodifiedSince;
    return _detail::BlobClient::SetImmutabilityPolicy(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::DeleteBlobImmutabilityPolicyResult> BlobClient::DeleteImmutabilityPolicy(
      const DeleteBlobImmutabilityPolicyOptions& options,
      const Azure::Core::Context& context) const
  {
    (void)options;
    _detail::BlobClient::DeleteBlobImmutabilityPolicyOptions protocolLayerOptions;
    return _detail::BlobClient::DeleteImmutabilityPolicy(
        *m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::SetBlobLegalHoldResult> BlobClient::SetLegalHold(
      bool hasLegalHold,
      const SetBlobLegalHoldOptions& options,
      const Azure::Core::Context& context) const
  {
    (void)options;
    _detail::BlobClient::SetBlobLegalHoldOptions protocolLayerOptions;
    protocolLayerOptions.LegalHold = hasLegalHold;
    return _detail::BlobClient::SetLegalHold(*m_pipeline, m_blobUrl, protocolLayerOptions, context);
  }

  Azure::Response<Models::AccountInfo> BlobClient::GetAccountInfo(
      const GetAccountInfoOptions& options,
      const Azure::Core::Context& context) const
  {
    (void)options;
    _detail::BlobClient::GetBlobAccountInfoOptions protocolLayerOptions;
    return _detail::BlobClient::GetAccountInfo(
        *m_pipeline, m_blobUrl, protocolLayerOptions, _internal::WithReplicaStatus(context));
  }

}}} // namespace Azure::Storage::Blobs
