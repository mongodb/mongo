/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/GetObjectResult.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Stream;
using namespace Aws::Utils;
using namespace Aws;

GetObjectResult::GetObjectResult() : 
    m_deleteMarker(false),
    m_contentLength(0),
    m_missingMeta(0),
    m_serverSideEncryption(ServerSideEncryption::NOT_SET),
    m_bucketKeyEnabled(false),
    m_storageClass(StorageClass::NOT_SET),
    m_requestCharged(RequestCharged::NOT_SET),
    m_replicationStatus(ReplicationStatus::NOT_SET),
    m_partsCount(0),
    m_tagCount(0),
    m_objectLockMode(ObjectLockMode::NOT_SET),
    m_objectLockLegalHoldStatus(ObjectLockLegalHoldStatus::NOT_SET)
{
}

GetObjectResult::GetObjectResult(GetObjectResult&& toMove) : 
    m_body(std::move(toMove.m_body)),
    m_deleteMarker(toMove.m_deleteMarker),
    m_acceptRanges(std::move(toMove.m_acceptRanges)),
    m_expiration(std::move(toMove.m_expiration)),
    m_restore(std::move(toMove.m_restore)),
    m_lastModified(std::move(toMove.m_lastModified)),
    m_contentLength(toMove.m_contentLength),
    m_eTag(std::move(toMove.m_eTag)),
    m_checksumCRC32(std::move(toMove.m_checksumCRC32)),
    m_checksumCRC32C(std::move(toMove.m_checksumCRC32C)),
    m_checksumSHA1(std::move(toMove.m_checksumSHA1)),
    m_checksumSHA256(std::move(toMove.m_checksumSHA256)),
    m_missingMeta(toMove.m_missingMeta),
    m_versionId(std::move(toMove.m_versionId)),
    m_cacheControl(std::move(toMove.m_cacheControl)),
    m_contentDisposition(std::move(toMove.m_contentDisposition)),
    m_contentEncoding(std::move(toMove.m_contentEncoding)),
    m_contentLanguage(std::move(toMove.m_contentLanguage)),
    m_contentRange(std::move(toMove.m_contentRange)),
    m_contentType(std::move(toMove.m_contentType)),
    m_expires(std::move(toMove.m_expires)),
    m_websiteRedirectLocation(std::move(toMove.m_websiteRedirectLocation)),
    m_serverSideEncryption(toMove.m_serverSideEncryption),
    m_metadata(std::move(toMove.m_metadata)),
    m_sSECustomerAlgorithm(std::move(toMove.m_sSECustomerAlgorithm)),
    m_sSECustomerKeyMD5(std::move(toMove.m_sSECustomerKeyMD5)),
    m_sSEKMSKeyId(std::move(toMove.m_sSEKMSKeyId)),
    m_bucketKeyEnabled(toMove.m_bucketKeyEnabled),
    m_storageClass(toMove.m_storageClass),
    m_requestCharged(toMove.m_requestCharged),
    m_replicationStatus(toMove.m_replicationStatus),
    m_partsCount(toMove.m_partsCount),
    m_tagCount(toMove.m_tagCount),
    m_objectLockMode(toMove.m_objectLockMode),
    m_objectLockRetainUntilDate(std::move(toMove.m_objectLockRetainUntilDate)),
    m_objectLockLegalHoldStatus(toMove.m_objectLockLegalHoldStatus),
    m_id2(std::move(toMove.m_id2)),
    m_requestId(std::move(toMove.m_requestId)),
    m_expiresString(std::move(toMove.m_expiresString))
{
}

GetObjectResult& GetObjectResult::operator=(GetObjectResult&& toMove)
{
   if(this == &toMove)
   {
      return *this;
   }

   m_body = std::move(toMove.m_body);
   m_deleteMarker = toMove.m_deleteMarker;
   m_acceptRanges = std::move(toMove.m_acceptRanges);
   m_expiration = std::move(toMove.m_expiration);
   m_restore = std::move(toMove.m_restore);
   m_lastModified = std::move(toMove.m_lastModified);
   m_contentLength = toMove.m_contentLength;
   m_eTag = std::move(toMove.m_eTag);
   m_checksumCRC32 = std::move(toMove.m_checksumCRC32);
   m_checksumCRC32C = std::move(toMove.m_checksumCRC32C);
   m_checksumSHA1 = std::move(toMove.m_checksumSHA1);
   m_checksumSHA256 = std::move(toMove.m_checksumSHA256);
   m_missingMeta = toMove.m_missingMeta;
   m_versionId = std::move(toMove.m_versionId);
   m_cacheControl = std::move(toMove.m_cacheControl);
   m_contentDisposition = std::move(toMove.m_contentDisposition);
   m_contentEncoding = std::move(toMove.m_contentEncoding);
   m_contentLanguage = std::move(toMove.m_contentLanguage);
   m_contentRange = std::move(toMove.m_contentRange);
   m_contentType = std::move(toMove.m_contentType);
   m_expires = std::move(toMove.m_expires);
   m_websiteRedirectLocation = std::move(toMove.m_websiteRedirectLocation);
   m_serverSideEncryption = toMove.m_serverSideEncryption;
   m_metadata = std::move(toMove.m_metadata);
   m_sSECustomerAlgorithm = std::move(toMove.m_sSECustomerAlgorithm);
   m_sSECustomerKeyMD5 = std::move(toMove.m_sSECustomerKeyMD5);
   m_sSEKMSKeyId = std::move(toMove.m_sSEKMSKeyId);
   m_bucketKeyEnabled = toMove.m_bucketKeyEnabled;
   m_storageClass = toMove.m_storageClass;
   m_requestCharged = toMove.m_requestCharged;
   m_replicationStatus = toMove.m_replicationStatus;
   m_partsCount = toMove.m_partsCount;
   m_tagCount = toMove.m_tagCount;
   m_objectLockMode = toMove.m_objectLockMode;
   m_objectLockRetainUntilDate = std::move(toMove.m_objectLockRetainUntilDate);
   m_objectLockLegalHoldStatus = toMove.m_objectLockLegalHoldStatus;
   m_id2 = std::move(toMove.m_id2);
   m_requestId = std::move(toMove.m_requestId);
   m_expiresString = std::move(toMove.m_expiresString);

   return *this;
}

GetObjectResult::GetObjectResult(Aws::AmazonWebServiceResult<ResponseStream>&& result)
  : GetObjectResult()
{
  *this = std::move(result);
}

GetObjectResult& GetObjectResult::operator =(Aws::AmazonWebServiceResult<ResponseStream>&& result)
{
  m_body = result.TakeOwnershipOfPayload();

  const auto& headers = result.GetHeaderValueCollection();
  const auto& deleteMarkerIter = headers.find("x-amz-delete-marker");
  if(deleteMarkerIter != headers.end())
  {
     m_deleteMarker = StringUtils::ConvertToBool(deleteMarkerIter->second.c_str());
  }

  const auto& acceptRangesIter = headers.find("accept-ranges");
  if(acceptRangesIter != headers.end())
  {
    m_acceptRanges = acceptRangesIter->second;
  }

  const auto& expirationIter = headers.find("x-amz-expiration");
  if(expirationIter != headers.end())
  {
    m_expiration = expirationIter->second;
  }

  const auto& restoreIter = headers.find("x-amz-restore");
  if(restoreIter != headers.end())
  {
    m_restore = restoreIter->second;
  }

  const auto& lastModifiedIter = headers.find("last-modified");
  if(lastModifiedIter != headers.end())
  {
    m_lastModified = DateTime(lastModifiedIter->second.c_str(), Aws::Utils::DateFormat::RFC822);
    if(!m_lastModified.WasParseSuccessful())
    {
      AWS_LOGSTREAM_WARN("S3::GetObjectResult", "Failed to parse lastModified header as an RFC822 timestamp: " << lastModifiedIter->second.c_str());
    }
  }

  const auto& contentLengthIter = headers.find("content-length");
  if(contentLengthIter != headers.end())
  {
     m_contentLength = StringUtils::ConvertToInt64(contentLengthIter->second.c_str());
  }

  const auto& eTagIter = headers.find("etag");
  if(eTagIter != headers.end())
  {
    m_eTag = eTagIter->second;
  }

  const auto& checksumCRC32Iter = headers.find("x-amz-checksum-crc32");
  if(checksumCRC32Iter != headers.end())
  {
    m_checksumCRC32 = checksumCRC32Iter->second;
  }

  const auto& checksumCRC32CIter = headers.find("x-amz-checksum-crc32c");
  if(checksumCRC32CIter != headers.end())
  {
    m_checksumCRC32C = checksumCRC32CIter->second;
  }

  const auto& checksumSHA1Iter = headers.find("x-amz-checksum-sha1");
  if(checksumSHA1Iter != headers.end())
  {
    m_checksumSHA1 = checksumSHA1Iter->second;
  }

  const auto& checksumSHA256Iter = headers.find("x-amz-checksum-sha256");
  if(checksumSHA256Iter != headers.end())
  {
    m_checksumSHA256 = checksumSHA256Iter->second;
  }

  const auto& missingMetaIter = headers.find("x-amz-missing-meta");
  if(missingMetaIter != headers.end())
  {
     m_missingMeta = StringUtils::ConvertToInt32(missingMetaIter->second.c_str());
  }

  const auto& versionIdIter = headers.find("x-amz-version-id");
  if(versionIdIter != headers.end())
  {
    m_versionId = versionIdIter->second;
  }

  const auto& cacheControlIter = headers.find("cache-control");
  if(cacheControlIter != headers.end())
  {
    m_cacheControl = cacheControlIter->second;
  }

  const auto& contentDispositionIter = headers.find("content-disposition");
  if(contentDispositionIter != headers.end())
  {
    m_contentDisposition = contentDispositionIter->second;
  }

  const auto& contentEncodingIter = headers.find("content-encoding");
  if(contentEncodingIter != headers.end())
  {
    m_contentEncoding = contentEncodingIter->second;
  }

  const auto& contentLanguageIter = headers.find("content-language");
  if(contentLanguageIter != headers.end())
  {
    m_contentLanguage = contentLanguageIter->second;
  }

  const auto& contentRangeIter = headers.find("content-range");
  if(contentRangeIter != headers.end())
  {
    m_contentRange = contentRangeIter->second;
  }

  const auto& contentTypeIter = headers.find("content-type");
  if(contentTypeIter != headers.end())
  {
    m_contentType = contentTypeIter->second;
  }

  const auto& expiresIter = headers.find("expires");
  if(expiresIter != headers.end())
  {
    m_expires = DateTime(expiresIter->second.c_str(), Aws::Utils::DateFormat::RFC822);
    if(!m_expires.WasParseSuccessful())
    {
      AWS_LOGSTREAM_WARN("S3::GetObjectResult", "Failed to parse expires header as an RFC822 timestamp: " << expiresIter->second.c_str());
    }
  }

  const auto& websiteRedirectLocationIter = headers.find("x-amz-website-redirect-location");
  if(websiteRedirectLocationIter != headers.end())
  {
    m_websiteRedirectLocation = websiteRedirectLocationIter->second;
  }

  const auto& serverSideEncryptionIter = headers.find("x-amz-server-side-encryption");
  if(serverSideEncryptionIter != headers.end())
  {
    m_serverSideEncryption = ServerSideEncryptionMapper::GetServerSideEncryptionForName(serverSideEncryptionIter->second);
  }

  std::size_t prefixSize = sizeof("x-amz-meta-") - 1; //subtract the NULL terminator out
  for(const auto& item : headers)
  {
    std::size_t foundPrefix = item.first.find("x-amz-meta-");

    if(foundPrefix != std::string::npos)
    {
      m_metadata[item.first.substr(prefixSize)] = item.second;
    }
  }

  const auto& sSECustomerAlgorithmIter = headers.find("x-amz-server-side-encryption-customer-algorithm");
  if(sSECustomerAlgorithmIter != headers.end())
  {
    m_sSECustomerAlgorithm = sSECustomerAlgorithmIter->second;
  }

  const auto& sSECustomerKeyMD5Iter = headers.find("x-amz-server-side-encryption-customer-key-md5");
  if(sSECustomerKeyMD5Iter != headers.end())
  {
    m_sSECustomerKeyMD5 = sSECustomerKeyMD5Iter->second;
  }

  const auto& sSEKMSKeyIdIter = headers.find("x-amz-server-side-encryption-aws-kms-key-id");
  if(sSEKMSKeyIdIter != headers.end())
  {
    m_sSEKMSKeyId = sSEKMSKeyIdIter->second;
  }

  const auto& bucketKeyEnabledIter = headers.find("x-amz-server-side-encryption-bucket-key-enabled");
  if(bucketKeyEnabledIter != headers.end())
  {
     m_bucketKeyEnabled = StringUtils::ConvertToBool(bucketKeyEnabledIter->second.c_str());
  }

  const auto& storageClassIter = headers.find("x-amz-storage-class");
  if(storageClassIter != headers.end())
  {
    m_storageClass = StorageClassMapper::GetStorageClassForName(storageClassIter->second);
  }

  const auto& requestChargedIter = headers.find("x-amz-request-charged");
  if(requestChargedIter != headers.end())
  {
    m_requestCharged = RequestChargedMapper::GetRequestChargedForName(requestChargedIter->second);
  }

  const auto& replicationStatusIter = headers.find("x-amz-replication-status");
  if(replicationStatusIter != headers.end())
  {
    m_replicationStatus = ReplicationStatusMapper::GetReplicationStatusForName(replicationStatusIter->second);
  }

  const auto& partsCountIter = headers.find("x-amz-mp-parts-count");
  if(partsCountIter != headers.end())
  {
     m_partsCount = StringUtils::ConvertToInt32(partsCountIter->second.c_str());
  }

  const auto& tagCountIter = headers.find("x-amz-tagging-count");
  if(tagCountIter != headers.end())
  {
     m_tagCount = StringUtils::ConvertToInt32(tagCountIter->second.c_str());
  }

  const auto& objectLockModeIter = headers.find("x-amz-object-lock-mode");
  if(objectLockModeIter != headers.end())
  {
    m_objectLockMode = ObjectLockModeMapper::GetObjectLockModeForName(objectLockModeIter->second);
  }

  const auto& objectLockRetainUntilDateIter = headers.find("x-amz-object-lock-retain-until-date");
  if(objectLockRetainUntilDateIter != headers.end())
  {
    m_objectLockRetainUntilDate = DateTime(objectLockRetainUntilDateIter->second.c_str(), Aws::Utils::DateFormat::ISO_8601);
    if(!m_objectLockRetainUntilDate.WasParseSuccessful())
    {
      AWS_LOGSTREAM_WARN("S3::GetObjectResult", "Failed to parse objectLockRetainUntilDate header as an ISO_8601 timestamp: " << objectLockRetainUntilDateIter->second.c_str());
    }
  }

  const auto& objectLockLegalHoldStatusIter = headers.find("x-amz-object-lock-legal-hold");
  if(objectLockLegalHoldStatusIter != headers.end())
  {
    m_objectLockLegalHoldStatus = ObjectLockLegalHoldStatusMapper::GetObjectLockLegalHoldStatusForName(objectLockLegalHoldStatusIter->second);
  }

  const auto& id2Iter = headers.find("x-amz-id-2");
  if(id2Iter != headers.end())
  {
    m_id2 = id2Iter->second;
  }

  const auto& requestIdIter = headers.find("x-amz-request-id");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }

  const auto& expiresStringIter = headers.find("expires");
  if(expiresStringIter != headers.end())
  {
    m_expiresString = expiresStringIter->second;
  }

   return *this;
}
