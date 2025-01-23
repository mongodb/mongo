/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/HeadObjectRequest.h>
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

HeadObjectRequest::HeadObjectRequest() : 
    m_bucketHasBeenSet(false),
    m_ifMatchHasBeenSet(false),
    m_ifModifiedSinceHasBeenSet(false),
    m_ifNoneMatchHasBeenSet(false),
    m_ifUnmodifiedSinceHasBeenSet(false),
    m_keyHasBeenSet(false),
    m_rangeHasBeenSet(false),
    m_responseCacheControlHasBeenSet(false),
    m_responseContentDispositionHasBeenSet(false),
    m_responseContentEncodingHasBeenSet(false),
    m_responseContentLanguageHasBeenSet(false),
    m_responseContentTypeHasBeenSet(false),
    m_responseExpiresHasBeenSet(false),
    m_versionIdHasBeenSet(false),
    m_sSECustomerAlgorithmHasBeenSet(false),
    m_sSECustomerKeyHasBeenSet(false),
    m_sSECustomerKeyMD5HasBeenSet(false),
    m_requestPayer(RequestPayer::NOT_SET),
    m_requestPayerHasBeenSet(false),
    m_partNumber(0),
    m_partNumberHasBeenSet(false),
    m_expectedBucketOwnerHasBeenSet(false),
    m_checksumMode(ChecksumMode::NOT_SET),
    m_checksumModeHasBeenSet(false),
    m_customizedAccessLogTagHasBeenSet(false)
{
}

bool HeadObjectRequest::HasEmbeddedError(Aws::IOStream &body,
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

Aws::String HeadObjectRequest::SerializePayload() const
{
  return {};
}

void HeadObjectRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_responseCacheControlHasBeenSet)
    {
      ss << m_responseCacheControl;
      uri.AddQueryStringParameter("response-cache-control", ss.str());
      ss.str("");
    }

    if(m_responseContentDispositionHasBeenSet)
    {
      ss << m_responseContentDisposition;
      uri.AddQueryStringParameter("response-content-disposition", ss.str());
      ss.str("");
    }

    if(m_responseContentEncodingHasBeenSet)
    {
      ss << m_responseContentEncoding;
      uri.AddQueryStringParameter("response-content-encoding", ss.str());
      ss.str("");
    }

    if(m_responseContentLanguageHasBeenSet)
    {
      ss << m_responseContentLanguage;
      uri.AddQueryStringParameter("response-content-language", ss.str());
      ss.str("");
    }

    if(m_responseContentTypeHasBeenSet)
    {
      ss << m_responseContentType;
      uri.AddQueryStringParameter("response-content-type", ss.str());
      ss.str("");
    }

    if(m_responseExpiresHasBeenSet)
    {
      ss << m_responseExpires.ToGmtString(Aws::Utils::DateFormat::RFC822);
      uri.AddQueryStringParameter("response-expires", ss.str());
      ss.str("");
    }

    if(m_versionIdHasBeenSet)
    {
      ss << m_versionId;
      uri.AddQueryStringParameter("versionId", ss.str());
      ss.str("");
    }

    if(m_partNumberHasBeenSet)
    {
      ss << m_partNumber;
      uri.AddQueryStringParameter("partNumber", ss.str());
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

Aws::Http::HeaderValueCollection HeadObjectRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  Aws::StringStream ss;
  if(m_ifMatchHasBeenSet)
  {
    ss << m_ifMatch;
    headers.emplace("if-match",  ss.str());
    ss.str("");
  }

  if(m_ifModifiedSinceHasBeenSet)
  {
    headers.emplace("if-modified-since", m_ifModifiedSince.ToGmtString(Aws::Utils::DateFormat::RFC822));
  }

  if(m_ifNoneMatchHasBeenSet)
  {
    ss << m_ifNoneMatch;
    headers.emplace("if-none-match",  ss.str());
    ss.str("");
  }

  if(m_ifUnmodifiedSinceHasBeenSet)
  {
    headers.emplace("if-unmodified-since", m_ifUnmodifiedSince.ToGmtString(Aws::Utils::DateFormat::RFC822));
  }

  if(m_rangeHasBeenSet)
  {
    ss << m_range;
    headers.emplace("range",  ss.str());
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

  if(m_checksumModeHasBeenSet && m_checksumMode != ChecksumMode::NOT_SET)
  {
    headers.emplace("x-amz-checksum-mode", ChecksumModeMapper::GetNameForChecksumMode(m_checksumMode));
  }

  return headers;
}

HeadObjectRequest::EndpointParameters HeadObjectRequest::GetEndpointContextParams() const
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
