/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ListDirectoryBucketsRequest.h>
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

ListDirectoryBucketsRequest::ListDirectoryBucketsRequest() : 
    m_continuationTokenHasBeenSet(false),
    m_maxDirectoryBuckets(0),
    m_maxDirectoryBucketsHasBeenSet(false),
    m_customizedAccessLogTagHasBeenSet(false)
{
}

bool ListDirectoryBucketsRequest::HasEmbeddedError(Aws::IOStream &body,
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

Aws::String ListDirectoryBucketsRequest::SerializePayload() const
{
  return {};
}

void ListDirectoryBucketsRequest::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_continuationTokenHasBeenSet)
    {
      ss << m_continuationToken;
      uri.AddQueryStringParameter("continuation-token", ss.str());
      ss.str("");
    }

    if(m_maxDirectoryBucketsHasBeenSet)
    {
      ss << m_maxDirectoryBuckets;
      uri.AddQueryStringParameter("max-directory-buckets", ss.str());
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


ListDirectoryBucketsRequest::EndpointParameters ListDirectoryBucketsRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Static context parameters
    parameters.emplace_back(Aws::String("UseS3ExpressControlEndpoint"), true, Aws::Endpoint::EndpointParameter::ParameterOrigin::STATIC_CONTEXT);
    return parameters;
}
