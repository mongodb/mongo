/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws::Http;

CopyObjectRequest::CopyObjectRequest() : 
    m_aCL(ObjectCannedACL::NOT_SET),
    m_aCLHasBeenSet(false),
    m_bucketHasBeenSet(false),
    m_cacheControlHasBeenSet(false),
    m_checksumAlgorithm(ChecksumAlgorithm::NOT_SET),
    m_checksumAlgorithmHasBeenSet(false),
    m_contentDispositionHasBeenSet(false),
    m_contentEncodingHasBeenSet(false),
    m_contentLanguageHasBeenSet(false),
    m_contentTypeHasBeenSet(false),
    m_copySourceHasBeenSet(false),
    m_copySourceIfMatchHasBeenSet(false),
    m_copySourceIfModifiedSinceHasBeenSet(false),
    m_copySourceIfNoneMatchHasBeenSet(false),
    m_copySourceIfUnmodifiedSinceHasBeenSet(false),
    m_expiresHasBeenSet(false),
    m_grantFullControlHasBeenSet(false),
    m_grantReadHasBeenSet(false),
    m_grantReadACPHasBeenSet(false),
    m_grantWriteACPHasBeenSet(false),
    m_keyHasBeenSet(false),
    m_metadataHasBeenSet(false),
    m_metadataDirective(MetadataDirective::NOT_SET),
    m_metadataDirectiveHasBeenSet(false),
    m_taggingDirective(TaggingDirective::NOT_SET),
    m_taggingDirectiveHasBeenSet(false),
    m_serverSideEncryption(ServerSideEncryption::NOT_SET),
    m_serverSideEncryptionHasBeenSet(false),
    m_storageClass(StorageClass::NOT_SET),
    m_storageClassHasBeenSet(false),
    m_websiteRedirectLocationHasBeenSet(false),
    m_sSECustomerAlgorithmHasBeenSet(false),
    m_sSECustomerKeyHasBeenSet(false),
    m_sSECustomerKeyMD5HasBeenSet(false),
    m_sSEKMSKeyIdHasBeenSet(false),
    m_sSEKMSEncryptionContextHasBeenSet(false),
    m_bucketKeyEnabled(false),
    m_bucketKeyEnabledHasBeenSet(false),
    m_copySourceSSECustomerAlgorithmHasBeenSet(false),
    m_copySourceSSECustomerKeyHasBeenSet(false),
    m_copySourceSSECustomerKeyMD5HasBeenSet(false),
    m_requestPayer(RequestPayer::NOT_SET),
    m_requestPayerHasBeenSet(false),
    m_taggingHasBeenSet(false),
    m_objectLockMode(ObjectLockMode::NOT_SET),
    m_objectLockModeHasBeenSet(false),
    m_objectLockRetainUntilDateHasBeenSet(false),
    m_objectLockLegalHoldStatus(ObjectLockLegalHoldStatus::NOT_SET),
    m_objectLockLegalHoldStatusHasBeenSet(false),
    m_expectedBucketOwnerHasBeenSet(false),
    m_expectedSourceBucketOwnerHasBeenSet(false),
    m_customizedAccessLogTagHasBeenSet(false)
{
}

bool CopyObjectRequest::HasEmbeddedError(Aws::IOStream &body,
  const Aws::Http::HeaderValueCollection &header) const
{
  // Header is unused
  AWS_UNREFERENCED_PARAM(header);

  auto readPointer = body.tellg();
  Utils::Xml::XmlDocument doc = XmlDocument::CreateFromXmlStream(body);
  body.seekg(readPointer);
  if (!doc.WasParseSuccessful()) {
    return false;
  }

  if (!doc.GetRootElement().IsNull() && doc.GetRootElement().GetName() == Aws::String("Error")) {
    return true;
  }
  return false;
}

Aws::String CopyObjectRequest::SerializePayload() const
{
  return {};
}

void CopyObjectRequest::AddQueryStringParameters(URI& uri) const
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

Aws::Http::HeaderValueCollection CopyObjectRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  Aws::StringStream ss;
  if(m_aCLHasBeenSet && m_aCL != ObjectCannedACL::NOT_SET)
  {
    headers.emplace("x-amz-acl", ObjectCannedACLMapper::GetNameForObjectCannedACL(m_aCL));
  }

  if(m_cacheControlHasBeenSet)
  {
    ss << m_cacheControl;
    headers.emplace("cache-control",  ss.str());
    ss.str("");
  }

  if(m_checksumAlgorithmHasBeenSet && m_checksumAlgorithm != ChecksumAlgorithm::NOT_SET)
  {
    headers.emplace("x-amz-checksum-algorithm", ChecksumAlgorithmMapper::GetNameForChecksumAlgorithm(m_checksumAlgorithm));
  }

  if(m_contentDispositionHasBeenSet)
  {
    ss << m_contentDisposition;
    headers.emplace("content-disposition",  ss.str());
    ss.str("");
  }

  if(m_contentEncodingHasBeenSet)
  {
    ss << m_contentEncoding;
    headers.emplace("content-encoding",  ss.str());
    ss.str("");
  }

  if(m_contentLanguageHasBeenSet)
  {
    ss << m_contentLanguage;
    headers.emplace("content-language",  ss.str());
    ss.str("");
  }

  if(m_contentTypeHasBeenSet)
  {
    ss << m_contentType;
    headers.emplace("content-type",  ss.str());
    ss.str("");
  }

  if(m_copySourceHasBeenSet)
  {
    ss << m_copySource;
    headers.emplace("x-amz-copy-source", URI::URLEncodePath(ss.str()));
    ss.str("");
  }

  if(m_copySourceIfMatchHasBeenSet)
  {
    ss << m_copySourceIfMatch;
    headers.emplace("x-amz-copy-source-if-match",  ss.str());
    ss.str("");
  }

  if(m_copySourceIfModifiedSinceHasBeenSet)
  {
    headers.emplace("x-amz-copy-source-if-modified-since", m_copySourceIfModifiedSince.ToGmtString(Aws::Utils::DateFormat::RFC822));
  }

  if(m_copySourceIfNoneMatchHasBeenSet)
  {
    ss << m_copySourceIfNoneMatch;
    headers.emplace("x-amz-copy-source-if-none-match",  ss.str());
    ss.str("");
  }

  if(m_copySourceIfUnmodifiedSinceHasBeenSet)
  {
    headers.emplace("x-amz-copy-source-if-unmodified-since", m_copySourceIfUnmodifiedSince.ToGmtString(Aws::Utils::DateFormat::RFC822));
  }

  if(m_expiresHasBeenSet)
  {
    headers.emplace("expires", m_expires.ToGmtString(Aws::Utils::DateFormat::RFC822));
  }

  if(m_grantFullControlHasBeenSet)
  {
    ss << m_grantFullControl;
    headers.emplace("x-amz-grant-full-control",  ss.str());
    ss.str("");
  }

  if(m_grantReadHasBeenSet)
  {
    ss << m_grantRead;
    headers.emplace("x-amz-grant-read",  ss.str());
    ss.str("");
  }

  if(m_grantReadACPHasBeenSet)
  {
    ss << m_grantReadACP;
    headers.emplace("x-amz-grant-read-acp",  ss.str());
    ss.str("");
  }

  if(m_grantWriteACPHasBeenSet)
  {
    ss << m_grantWriteACP;
    headers.emplace("x-amz-grant-write-acp",  ss.str());
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

  if(m_metadataDirectiveHasBeenSet && m_metadataDirective != MetadataDirective::NOT_SET)
  {
    headers.emplace("x-amz-metadata-directive", MetadataDirectiveMapper::GetNameForMetadataDirective(m_metadataDirective));
  }

  if(m_taggingDirectiveHasBeenSet && m_taggingDirective != TaggingDirective::NOT_SET)
  {
    headers.emplace("x-amz-tagging-directive", TaggingDirectiveMapper::GetNameForTaggingDirective(m_taggingDirective));
  }

  if(m_serverSideEncryptionHasBeenSet && m_serverSideEncryption != ServerSideEncryption::NOT_SET)
  {
    headers.emplace("x-amz-server-side-encryption", ServerSideEncryptionMapper::GetNameForServerSideEncryption(m_serverSideEncryption));
  }

  if(m_storageClassHasBeenSet && m_storageClass != StorageClass::NOT_SET)
  {
    headers.emplace("x-amz-storage-class", StorageClassMapper::GetNameForStorageClass(m_storageClass));
  }

  if(m_websiteRedirectLocationHasBeenSet)
  {
    ss << m_websiteRedirectLocation;
    headers.emplace("x-amz-website-redirect-location",  ss.str());
    ss.str("");
  }

  if(m_sSECustomerAlgorithmHasBeenSet)
  {
    ss << m_sSECustomerAlgorithm;
    headers.emplace("x-amz-server-side-encryption-customer-algorithm",  ss.str());
    ss.str("");
  }

  if(m_sSECustomerKeyHasBeenSet)
  {
    ss << m_sSECustomerKey;
    headers.emplace("x-amz-server-side-encryption-customer-key",  ss.str());
    ss.str("");
  }

  if(m_sSECustomerKeyMD5HasBeenSet)
  {
    ss << m_sSECustomerKeyMD5;
    headers.emplace("x-amz-server-side-encryption-customer-key-md5",  ss.str());
    ss.str("");
  }

  if(m_sSEKMSKeyIdHasBeenSet)
  {
    ss << m_sSEKMSKeyId;
    headers.emplace("x-amz-server-side-encryption-aws-kms-key-id",  ss.str());
    ss.str("");
  }

  if(m_sSEKMSEncryptionContextHasBeenSet)
  {
    ss << m_sSEKMSEncryptionContext;
    headers.emplace("x-amz-server-side-encryption-context",  ss.str());
    ss.str("");
  }

  if(m_bucketKeyEnabledHasBeenSet)
  {
    ss << std::boolalpha << m_bucketKeyEnabled;
    headers.emplace("x-amz-server-side-encryption-bucket-key-enabled", ss.str());
    ss.str("");
  }

  if(m_copySourceSSECustomerAlgorithmHasBeenSet)
  {
    ss << m_copySourceSSECustomerAlgorithm;
    headers.emplace("x-amz-copy-source-server-side-encryption-customer-algorithm",  ss.str());
    ss.str("");
  }

  if(m_copySourceSSECustomerKeyHasBeenSet)
  {
    ss << m_copySourceSSECustomerKey;
    headers.emplace("x-amz-copy-source-server-side-encryption-customer-key",  ss.str());
    ss.str("");
  }

  if(m_copySourceSSECustomerKeyMD5HasBeenSet)
  {
    ss << m_copySourceSSECustomerKeyMD5;
    headers.emplace("x-amz-copy-source-server-side-encryption-customer-key-md5",  ss.str());
    ss.str("");
  }

  if(m_requestPayerHasBeenSet && m_requestPayer != RequestPayer::NOT_SET)
  {
    headers.emplace("x-amz-request-payer", RequestPayerMapper::GetNameForRequestPayer(m_requestPayer));
  }

  if(m_taggingHasBeenSet)
  {
    ss << m_tagging;
    headers.emplace("x-amz-tagging",  ss.str());
    ss.str("");
  }

  if(m_objectLockModeHasBeenSet && m_objectLockMode != ObjectLockMode::NOT_SET)
  {
    headers.emplace("x-amz-object-lock-mode", ObjectLockModeMapper::GetNameForObjectLockMode(m_objectLockMode));
  }

  if(m_objectLockRetainUntilDateHasBeenSet)
  {
    headers.emplace("x-amz-object-lock-retain-until-date", m_objectLockRetainUntilDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
  }

  if(m_objectLockLegalHoldStatusHasBeenSet && m_objectLockLegalHoldStatus != ObjectLockLegalHoldStatus::NOT_SET)
  {
    headers.emplace("x-amz-object-lock-legal-hold", ObjectLockLegalHoldStatusMapper::GetNameForObjectLockLegalHoldStatus(m_objectLockLegalHoldStatus));
  }

  if(m_expectedBucketOwnerHasBeenSet)
  {
    ss << m_expectedBucketOwner;
    headers.emplace("x-amz-expected-bucket-owner",  ss.str());
    ss.str("");
  }

  if(m_expectedSourceBucketOwnerHasBeenSet)
  {
    ss << m_expectedSourceBucketOwner;
    headers.emplace("x-amz-source-expected-bucket-owner",  ss.str());
    ss.str("");
  }

  return headers;
}

CopyObjectRequest::EndpointParameters CopyObjectRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Static context parameters
    parameters.emplace_back(Aws::String("DisableS3ExpressSessionAuth"), true, Aws::Endpoint::EndpointParameter::ParameterOrigin::STATIC_CONTEXT);
    // Operation context parameters
    if (BucketHasBeenSet()) {
        parameters.emplace_back(Aws::String("Bucket"), this->GetBucket(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    if (CopySourceHasBeenSet()) {
        parameters.emplace_back(Aws::String("CopySource"), this->GetCopySource(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    if (KeyHasBeenSet()) {
        parameters.emplace_back(Aws::String("Key"), this->GetKey(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    return parameters;
}
