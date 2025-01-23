/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>
#include <numeric>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws::Http;

ListObjectsV2Request::ListObjectsV2Request() : 
    m_bucketHasBeenSet(false),
    m_delimiterHasBeenSet(false),
    m_encodingType(EncodingType::NOT_SET),
    m_encodingTypeHasBeenSet(false),
    m_maxKeys(0),
    m_maxKeysHasBeenSet(false),
    m_prefixHasBeenSet(false),
    m_continuationTokenHasBeenSet(false),
    m_fetchOwner(false),
    m_fetchOwnerHasBeenSet(false),
    m_startAfterHasBeenSet(false),
    m_requestPayer(RequestPayer::NOT_SET),
    m_requestPayerHasBeenSet(false),
    m_expectedBucketOwnerHasBeenSet(false),
    m_optionalObjectAttributesHasBeenSet(false),
    m_customizedAccessLogTagHasBeenSet(false)
{
}

bool ListObjectsV2Request::HasEmbeddedError(Aws::IOStream &body,
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

Aws::String ListObjectsV2Request::SerializePayload() const
{
  return {};
}

void ListObjectsV2Request::AddQueryStringParameters(URI& uri) const
{
    Aws::StringStream ss;
    if(m_delimiterHasBeenSet)
    {
      ss << m_delimiter;
      uri.AddQueryStringParameter("delimiter", ss.str());
      ss.str("");
    }

    if(m_encodingTypeHasBeenSet)
    {
      ss << EncodingTypeMapper::GetNameForEncodingType(m_encodingType);
      uri.AddQueryStringParameter("encoding-type", ss.str());
      ss.str("");
    }

    if(m_maxKeysHasBeenSet)
    {
      ss << m_maxKeys;
      uri.AddQueryStringParameter("max-keys", ss.str());
      ss.str("");
    }

    if(m_prefixHasBeenSet)
    {
      ss << m_prefix;
      uri.AddQueryStringParameter("prefix", ss.str());
      ss.str("");
    }

    if(m_continuationTokenHasBeenSet)
    {
      ss << m_continuationToken;
      uri.AddQueryStringParameter("continuation-token", ss.str());
      ss.str("");
    }

    if(m_fetchOwnerHasBeenSet)
    {
      ss << m_fetchOwner;
      uri.AddQueryStringParameter("fetch-owner", ss.str());
      ss.str("");
    }

    if(m_startAfterHasBeenSet)
    {
      ss << m_startAfter;
      uri.AddQueryStringParameter("start-after", ss.str());
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

Aws::Http::HeaderValueCollection ListObjectsV2Request::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  Aws::StringStream ss;
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

  if(m_optionalObjectAttributesHasBeenSet)
  {
    headers.emplace("x-amz-optional-object-attributes", std::accumulate(std::begin(m_optionalObjectAttributes),
      std::end(m_optionalObjectAttributes),
      Aws::String{},
      [](const Aws::String &acc, const OptionalObjectAttributes &item) -> Aws::String {
        const auto headerValue = OptionalObjectAttributesMapper::GetNameForOptionalObjectAttributes(item);
        return acc.empty() ? headerValue : acc + "," + headerValue;
      }));
  }

  return headers;
}

ListObjectsV2Request::EndpointParameters ListObjectsV2Request::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Operation context parameters
    if (BucketHasBeenSet()) {
        parameters.emplace_back(Aws::String("Bucket"), this->GetBucket(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    if (PrefixHasBeenSet()) {
        parameters.emplace_back(Aws::String("Prefix"), this->GetPrefix(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    return parameters;
}
