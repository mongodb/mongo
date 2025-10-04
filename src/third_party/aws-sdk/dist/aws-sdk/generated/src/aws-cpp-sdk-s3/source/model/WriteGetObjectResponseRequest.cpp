/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/WriteGetObjectResponseRequest.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/HashingUtils.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Stream;
using namespace Aws::Utils;
using namespace Aws::Http;
using namespace Aws;

WriteGetObjectResponseRequest::WriteGetObjectResponseRequest() : 
    m_requestRouteHasBeenSet(false),
    m_requestTokenHasBeenSet(false),
    m_statusCode(0),
    m_statusCodeHasBeenSet(false),
    m_errorCodeHasBeenSet(false),
    m_errorMessageHasBeenSet(false),
    m_acceptRangesHasBeenSet(false),
    m_cacheControlHasBeenSet(false),
    m_contentDispositionHasBeenSet(false),
    m_contentEncodingHasBeenSet(false),
    m_contentLanguageHasBeenSet(false),
    m_contentLength(0),
    m_contentLengthHasBeenSet(false),
    m_contentRangeHasBeenSet(false),
    m_checksumCRC32HasBeenSet(false),
    m_checksumCRC32CHasBeenSet(false),
    m_checksumSHA1HasBeenSet(false),
    m_checksumSHA256HasBeenSet(false),
    m_deleteMarker(false),
    m_deleteMarkerHasBeenSet(false),
    m_eTagHasBeenSet(false),
    m_expiresHasBeenSet(false),
    m_expirationHasBeenSet(false),
    m_lastModifiedHasBeenSet(false),
    m_missingMeta(0),
    m_missingMetaHasBeenSet(false),
    m_metadataHasBeenSet(false),
    m_objectLockMode(ObjectLockMode::NOT_SET),
    m_objectLockModeHasBeenSet(false),
    m_objectLockLegalHoldStatus(ObjectLockLegalHoldStatus::NOT_SET),
    m_objectLockLegalHoldStatusHasBeenSet(false),
    m_objectLockRetainUntilDateHasBeenSet(false),
    m_partsCount(0),
    m_partsCountHasBeenSet(false),
    m_replicationStatus(ReplicationStatus::NOT_SET),
    m_replicationStatusHasBeenSet(false),
    m_requestCharged(RequestCharged::NOT_SET),
    m_requestChargedHasBeenSet(false),
    m_restoreHasBeenSet(false),
    m_serverSideEncryption(ServerSideEncryption::NOT_SET),
    m_serverSideEncryptionHasBeenSet(false),
    m_sSECustomerAlgorithmHasBeenSet(false),
    m_sSEKMSKeyIdHasBeenSet(false),
    m_sSECustomerKeyMD5HasBeenSet(false),
    m_storageClass(StorageClass::NOT_SET),
    m_storageClassHasBeenSet(false),
    m_tagCount(0),
    m_tagCountHasBeenSet(false),
    m_versionIdHasBeenSet(false),
    m_bucketKeyEnabled(false),
    m_bucketKeyEnabledHasBeenSet(false),
    m_customizedAccessLogTagHasBeenSet(false)
{
}


void WriteGetObjectResponseRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(!m_customizedAccessLogTag.empty())
    {
        // only accept customized LogTag which starts with "x-"
        Aws::Map<Aws::String, Aws::String> collectedLogTags;
        for(const auto& entry: m_customizedAccessLogTag)
        {
            if (!entry.first.empty() && !entry.second.empty() && entry.first.substr(0, 2) == "x-")
            {
                collectedLogTags.emplace(entry.first, entry.second);
            }
        }

        if (!collectedLogTags.empty())
        {
            uri.AddQueryStringParameter(collectedLogTags);
        }
    }
}

Aws::Http::HeaderValueCollection WriteGetObjectResponseRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  Aws::StringStream ss;
  if(m_requestRouteHasBeenSet)
  {
    ss << m_requestRoute;
    headers.emplace("x-amz-request-route",  ss.str());
    ss.str("");
  }

  if(m_requestTokenHasBeenSet)
  {
    ss << m_requestToken;
    headers.emplace("x-amz-request-token",  ss.str());
    ss.str("");
  }

  if(m_statusCodeHasBeenSet)
  {
    ss << m_statusCode;
    headers.emplace("x-amz-fwd-status",  ss.str());
    ss.str("");
  }

  if(m_errorCodeHasBeenSet)
  {
    ss << m_errorCode;
    headers.emplace("x-amz-fwd-error-code",  ss.str());
    ss.str("");
  }

  if(m_errorMessageHasBeenSet)
  {
    ss << m_errorMessage;
    headers.emplace("x-amz-fwd-error-message",  ss.str());
    ss.str("");
  }

  if(m_acceptRangesHasBeenSet)
  {
    ss << m_acceptRanges;
    headers.emplace("x-amz-fwd-header-accept-ranges",  ss.str());
    ss.str("");
  }

  if(m_cacheControlHasBeenSet)
  {
    ss << m_cacheControl;
    headers.emplace("x-amz-fwd-header-cache-control",  ss.str());
    ss.str("");
  }

  if(m_contentDispositionHasBeenSet)
  {
    ss << m_contentDisposition;
    headers.emplace("x-amz-fwd-header-content-disposition",  ss.str());
    ss.str("");
  }

  if(m_contentEncodingHasBeenSet)
  {
    ss << m_contentEncoding;
    headers.emplace("x-amz-fwd-header-content-encoding",  ss.str());
    ss.str("");
  }

  if(m_contentLanguageHasBeenSet)
  {
    ss << m_contentLanguage;
    headers.emplace("x-amz-fwd-header-content-language",  ss.str());
    ss.str("");
  }

  if(m_contentLengthHasBeenSet)
  {
    ss << m_contentLength;
    headers.emplace("content-length",  ss.str());
    ss.str("");
  }

  if(m_contentRangeHasBeenSet)
  {
    ss << m_contentRange;
    headers.emplace("x-amz-fwd-header-content-range",  ss.str());
    ss.str("");
  }

  if(m_checksumCRC32HasBeenSet)
  {
    ss << m_checksumCRC32;
    headers.emplace("x-amz-fwd-header-x-amz-checksum-crc32",  ss.str());
    ss.str("");
  }

  if(m_checksumCRC32CHasBeenSet)
  {
    ss << m_checksumCRC32C;
    headers.emplace("x-amz-fwd-header-x-amz-checksum-crc32c",  ss.str());
    ss.str("");
  }

  if(m_checksumSHA1HasBeenSet)
  {
    ss << m_checksumSHA1;
    headers.emplace("x-amz-fwd-header-x-amz-checksum-sha1",  ss.str());
    ss.str("");
  }

  if(m_checksumSHA256HasBeenSet)
  {
    ss << m_checksumSHA256;
    headers.emplace("x-amz-fwd-header-x-amz-checksum-sha256",  ss.str());
    ss.str("");
  }

  if(m_deleteMarkerHasBeenSet)
  {
    ss << std::boolalpha << m_deleteMarker;
    headers.emplace("x-amz-fwd-header-x-amz-delete-marker", ss.str());
    ss.str("");
  }

  if(m_eTagHasBeenSet)
  {
    ss << m_eTag;
    headers.emplace("x-amz-fwd-header-etag",  ss.str());
    ss.str("");
  }

  if(m_expiresHasBeenSet)
  {
    headers.emplace("x-amz-fwd-header-expires", m_expires.ToGmtString(Aws::Utils::DateFormat::RFC822));
  }

  if(m_expirationHasBeenSet)
  {
    ss << m_expiration;
    headers.emplace("x-amz-fwd-header-x-amz-expiration",  ss.str());
    ss.str("");
  }

  if(m_lastModifiedHasBeenSet)
  {
    headers.emplace("x-amz-fwd-header-last-modified", m_lastModified.ToGmtString(Aws::Utils::DateFormat::RFC822));
  }

  if(m_missingMetaHasBeenSet)
  {
    ss << m_missingMeta;
    headers.emplace("x-amz-fwd-header-x-amz-missing-meta",  ss.str());
    ss.str("");
  }

  if(m_metadataHasBeenSet)
  {
    for(const auto& item : m_metadata)
    {
      ss << "x-amz-meta-" << item.first;
      headers.emplace(ss.str(), item.second);
      ss.str("");
    }
  }

  if(m_objectLockModeHasBeenSet && m_objectLockMode != ObjectLockMode::NOT_SET)
  {
    headers.emplace("x-amz-fwd-header-x-amz-object-lock-mode", ObjectLockModeMapper::GetNameForObjectLockMode(m_objectLockMode));
  }

  if(m_objectLockLegalHoldStatusHasBeenSet && m_objectLockLegalHoldStatus != ObjectLockLegalHoldStatus::NOT_SET)
  {
    headers.emplace("x-amz-fwd-header-x-amz-object-lock-legal-hold", ObjectLockLegalHoldStatusMapper::GetNameForObjectLockLegalHoldStatus(m_objectLockLegalHoldStatus));
  }

  if(m_objectLockRetainUntilDateHasBeenSet)
  {
    headers.emplace("x-amz-fwd-header-x-amz-object-lock-retain-until-date", m_objectLockRetainUntilDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
  }

  if(m_partsCountHasBeenSet)
  {
    ss << m_partsCount;
    headers.emplace("x-amz-fwd-header-x-amz-mp-parts-count",  ss.str());
    ss.str("");
  }

  if(m_replicationStatusHasBeenSet && m_replicationStatus != ReplicationStatus::NOT_SET)
  {
    headers.emplace("x-amz-fwd-header-x-amz-replication-status", ReplicationStatusMapper::GetNameForReplicationStatus(m_replicationStatus));
  }

  if(m_requestChargedHasBeenSet && m_requestCharged != RequestCharged::NOT_SET)
  {
    headers.emplace("x-amz-fwd-header-x-amz-request-charged", RequestChargedMapper::GetNameForRequestCharged(m_requestCharged));
  }

  if(m_restoreHasBeenSet)
  {
    ss << m_restore;
    headers.emplace("x-amz-fwd-header-x-amz-restore",  ss.str());
    ss.str("");
  }

  if(m_serverSideEncryptionHasBeenSet && m_serverSideEncryption != ServerSideEncryption::NOT_SET)
  {
    headers.emplace("x-amz-fwd-header-x-amz-server-side-encryption", ServerSideEncryptionMapper::GetNameForServerSideEncryption(m_serverSideEncryption));
  }

  if(m_sSECustomerAlgorithmHasBeenSet)
  {
    ss << m_sSECustomerAlgorithm;
    headers.emplace("x-amz-fwd-header-x-amz-server-side-encryption-customer-algorithm",  ss.str());
    ss.str("");
  }

  if(m_sSEKMSKeyIdHasBeenSet)
  {
    ss << m_sSEKMSKeyId;
    headers.emplace("x-amz-fwd-header-x-amz-server-side-encryption-aws-kms-key-id",  ss.str());
    ss.str("");
  }

  if(m_sSECustomerKeyMD5HasBeenSet)
  {
    ss << m_sSECustomerKeyMD5;
    headers.emplace("x-amz-fwd-header-x-amz-server-side-encryption-customer-key-md5",  ss.str());
    ss.str("");
  }

  if(m_storageClassHasBeenSet && m_storageClass != StorageClass::NOT_SET)
  {
    headers.emplace("x-amz-fwd-header-x-amz-storage-class", StorageClassMapper::GetNameForStorageClass(m_storageClass));
  }

  if(m_tagCountHasBeenSet)
  {
    ss << m_tagCount;
    headers.emplace("x-amz-fwd-header-x-amz-tagging-count",  ss.str());
    ss.str("");
  }

  if(m_versionIdHasBeenSet)
  {
    ss << m_versionId;
    headers.emplace("x-amz-fwd-header-x-amz-version-id",  ss.str());
    ss.str("");
  }

  if(m_bucketKeyEnabledHasBeenSet)
  {
    ss << std::boolalpha << m_bucketKeyEnabled;
    headers.emplace("x-amz-fwd-header-x-amz-server-side-encryption-bucket-key-enabled", ss.str());
    ss.str("");
  }

  return headers;

}

bool WriteGetObjectResponseRequest::HasEmbeddedError(Aws::IOStream &body,
  const Aws::Http::HeaderValueCollection &header) const
{
  // Header is unused
  AWS_UNREFERENCED_PARAM(header);

  auto readPointer = body.tellg();
  Utils::Xml::XmlDocument doc = Utils::Xml::XmlDocument::CreateFromXmlStream(body);
  body.seekg(readPointer);

  if (!doc.WasParseSuccessful()) {
    return false;
  }

  if (!doc.GetRootElement().IsNull() && doc.GetRootElement().GetName() == Aws::String("Error")) {
    return true;
  }

  return false;
}

WriteGetObjectResponseRequest::EndpointParameters WriteGetObjectResponseRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Static context parameters
    parameters.emplace_back(Aws::String("UseObjectLambdaEndpoint"), true, Aws::Endpoint::EndpointParameter::ParameterOrigin::STATIC_CONTEXT);
    return parameters;
}
