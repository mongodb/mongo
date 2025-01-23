/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/PutBucketVersioningRequest.h>
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

PutBucketVersioningRequest::PutBucketVersioningRequest() : 
    m_bucketHasBeenSet(false),
    m_contentMD5HasBeenSet(false),
    m_checksumAlgorithm(ChecksumAlgorithm::NOT_SET),
    m_checksumAlgorithmHasBeenSet(false),
    m_mFAHasBeenSet(false),
    m_versioningConfigurationHasBeenSet(false),
    m_expectedBucketOwnerHasBeenSet(false),
    m_customizedAccessLogTagHasBeenSet(false)
{
}

bool PutBucketVersioningRequest::HasEmbeddedError(Aws::IOStream &body,
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

Aws::String PutBucketVersioningRequest::SerializePayload() const
{
  XmlDocument payloadDoc = XmlDocument::CreateWithRootNode("VersioningConfiguration");

  XmlNode parentNode = payloadDoc.GetRootElement();
  parentNode.SetAttributeValue("xmlns", "http://s3.amazonaws.com/doc/2006-03-01/");

  m_versioningConfiguration.AddToNode(parentNode);
  if(parentNode.HasChildren())
  {
    return payloadDoc.ConvertToString();
  }

  return {};
}

void PutBucketVersioningRequest::AddQueryStringParameters(URI& uri) const
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

Aws::Http::HeaderValueCollection PutBucketVersioningRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  Aws::StringStream ss;
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

  if(m_mFAHasBeenSet)
  {
    ss << m_mFA;
    headers.emplace("x-amz-mfa",  ss.str());
    ss.str("");
  }

  if(m_expectedBucketOwnerHasBeenSet)
  {
    ss << m_expectedBucketOwner;
    headers.emplace("x-amz-expected-bucket-owner",  ss.str());
    ss.str("");
  }

  return headers;
}

PutBucketVersioningRequest::EndpointParameters PutBucketVersioningRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Static context parameters
    parameters.emplace_back(Aws::String("UseS3ExpressControlEndpoint"), true, Aws::Endpoint::EndpointParameter::ParameterOrigin::STATIC_CONTEXT);
    // Operation context parameters
    if (BucketHasBeenSet()) {
        parameters.emplace_back(Aws::String("Bucket"), this->GetBucket(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    return parameters;
}

Aws::String PutBucketVersioningRequest::GetChecksumAlgorithmName() const
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

