/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/UploadPartRequest.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Stream;
using namespace Aws::Utils;
using namespace Aws::Http;
using namespace Aws;

UploadPartRequest::UploadPartRequest() : 
    m_bucketHasBeenSet(false),
    m_contentLength(0),
    m_contentLengthHasBeenSet(false),
    m_contentMD5HasBeenSet(false),
    m_checksumAlgorithm(ChecksumAlgorithm::NOT_SET),
    m_checksumAlgorithmHasBeenSet(false),
    m_checksumCRC32HasBeenSet(false),
    m_checksumCRC32CHasBeenSet(false),
    m_checksumSHA1HasBeenSet(false),
    m_checksumSHA256HasBeenSet(false),
    m_keyHasBeenSet(false),
    m_partNumber(0),
    m_partNumberHasBeenSet(false),
    m_uploadIdHasBeenSet(false),
    m_sSECustomerAlgorithmHasBeenSet(false),
    m_sSECustomerKeyHasBeenSet(false),
    m_sSECustomerKeyMD5HasBeenSet(false),
    m_requestPayer(RequestPayer::NOT_SET),
    m_requestPayerHasBeenSet(false),
    m_expectedBucketOwnerHasBeenSet(false),
    m_customizedAccessLogTagHasBeenSet(false)
{
}


void UploadPartRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_partNumberHasBeenSet)
    {
      ss << m_partNumber;
      uri.AddQueryStringParameter("partNumber", ss.str());
      ss.str("");
    }

    if(m_uploadIdHasBeenSet)
    {
      ss << m_uploadId;
      uri.AddQueryStringParameter("uploadId", ss.str());
      ss.str("");
    }

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

Aws::Http::HeaderValueCollection UploadPartRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  Aws::StringStream ss;
  if(m_contentLengthHasBeenSet)
  {
    ss << m_contentLength;
    headers.emplace("content-length",  ss.str());
    ss.str("");
  }

  if(m_contentMD5HasBeenSet)
  {
    ss << m_contentMD5;
    headers.emplace("content-md5",  ss.str());
    ss.str("");
  }

  if(m_checksumAlgorithmHasBeenSet && m_checksumAlgorithm != ChecksumAlgorithm::NOT_SET)
  {
    headers.emplace("x-amz-sdk-checksum-algorithm", ChecksumAlgorithmMapper::GetNameForChecksumAlgorithm(m_checksumAlgorithm));
  }

  if(m_checksumCRC32HasBeenSet)
  {
    ss << m_checksumCRC32;
    headers.emplace("x-amz-checksum-crc32",  ss.str());
    ss.str("");
  }

  if(m_checksumCRC32CHasBeenSet)
  {
    ss << m_checksumCRC32C;
    headers.emplace("x-amz-checksum-crc32c",  ss.str());
    ss.str("");
  }

  if(m_checksumSHA1HasBeenSet)
  {
    ss << m_checksumSHA1;
    headers.emplace("x-amz-checksum-sha1",  ss.str());
    ss.str("");
  }

  if(m_checksumSHA256HasBeenSet)
  {
    ss << m_checksumSHA256;
    headers.emplace("x-amz-checksum-sha256",  ss.str());
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

  if(m_requestPayerHasBeenSet && m_requestPayer != RequestPayer::NOT_SET)
  {
    headers.emplace("x-amz-request-payer", RequestPayerMapper::GetNameForRequestPayer(m_requestPayer));
  }

  if(m_expectedBucketOwnerHasBeenSet)
  {
    ss << m_expectedBucketOwner;
    headers.emplace("x-amz-expected-bucket-owner",  ss.str());
    ss.str("");
  }

  return headers;

}

bool UploadPartRequest::HasEmbeddedError(Aws::IOStream &body,
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

UploadPartRequest::EndpointParameters UploadPartRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Operation context parameters
    if (BucketHasBeenSet()) {
        parameters.emplace_back(Aws::String("Bucket"), this->GetBucket(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    if (KeyHasBeenSet()) {
        parameters.emplace_back(Aws::String("Key"), this->GetKey(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    return parameters;
}

Aws::String UploadPartRequest::GetChecksumAlgorithmName() const
{
  if (m_checksumAlgorithm == ChecksumAlgorithm::NOT_SET)
  {
    return "md5";
  }
  else
  {
    return ChecksumAlgorithmMapper::GetNameForChecksumAlgorithm(m_checksumAlgorithm);
  }
}

