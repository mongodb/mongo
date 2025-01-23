/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/DeleteObjectRequest.h>
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

DeleteObjectRequest::DeleteObjectRequest() : 
    m_bucketHasBeenSet(false),
    m_keyHasBeenSet(false),
    m_mFAHasBeenSet(false),
    m_versionIdHasBeenSet(false),
    m_requestPayer(RequestPayer::NOT_SET),
    m_requestPayerHasBeenSet(false),
    m_bypassGovernanceRetention(false),
    m_bypassGovernanceRetentionHasBeenSet(false),
    m_expectedBucketOwnerHasBeenSet(false),
    m_ifMatchHasBeenSet(false),
    m_ifMatchLastModifiedTimeHasBeenSet(false),
    m_ifMatchSize(0),
    m_ifMatchSizeHasBeenSet(false),
    m_customizedAccessLogTagHasBeenSet(false)
{
}

bool DeleteObjectRequest::HasEmbeddedError(Aws::IOStream &body,
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

Aws::String DeleteObjectRequest::SerializePayload() const
{
  return {};
}

void DeleteObjectRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_versionIdHasBeenSet)
    {
      ss << m_versionId;
      uri.AddQueryStringParameter("versionId", ss.str());
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

Aws::Http::HeaderValueCollection DeleteObjectRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  Aws::StringStream ss;
  if(m_mFAHasBeenSet)
  {
    ss << m_mFA;
    headers.emplace("x-amz-mfa",  ss.str());
    ss.str("");
  }

  if(m_requestPayerHasBeenSet && m_requestPayer != RequestPayer::NOT_SET)
  {
    headers.emplace("x-amz-request-payer", RequestPayerMapper::GetNameForRequestPayer(m_requestPayer));
  }

  if(m_bypassGovernanceRetentionHasBeenSet)
  {
    ss << std::boolalpha << m_bypassGovernanceRetention;
    headers.emplace("x-amz-bypass-governance-retention", ss.str());
    ss.str("");
  }

  if(m_expectedBucketOwnerHasBeenSet)
  {
    ss << m_expectedBucketOwner;
    headers.emplace("x-amz-expected-bucket-owner",  ss.str());
    ss.str("");
  }

  if(m_ifMatchHasBeenSet)
  {
    ss << m_ifMatch;
    headers.emplace("if-match",  ss.str());
    ss.str("");
  }

  if(m_ifMatchLastModifiedTimeHasBeenSet)
  {
    headers.emplace("x-amz-if-match-last-modified-time", m_ifMatchLastModifiedTime.ToGmtString(Aws::Utils::DateFormat::RFC822));
  }

  if(m_ifMatchSizeHasBeenSet)
  {
    ss << m_ifMatchSize;
    headers.emplace("x-amz-if-match-size",  ss.str());
    ss.str("");
  }

  return headers;
}

DeleteObjectRequest::EndpointParameters DeleteObjectRequest::GetEndpointContextParams() const
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
