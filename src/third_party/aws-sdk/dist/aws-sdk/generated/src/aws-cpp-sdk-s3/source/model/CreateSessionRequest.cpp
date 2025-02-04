/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/CreateSessionRequest.h>
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

CreateSessionRequest::CreateSessionRequest() : 
    m_sessionMode(SessionMode::NOT_SET),
    m_sessionModeHasBeenSet(false),
    m_bucketHasBeenSet(false),
    m_serverSideEncryption(ServerSideEncryption::NOT_SET),
    m_serverSideEncryptionHasBeenSet(false),
    m_sSEKMSKeyIdHasBeenSet(false),
    m_sSEKMSEncryptionContextHasBeenSet(false),
    m_bucketKeyEnabled(false),
    m_bucketKeyEnabledHasBeenSet(false),
    m_customizedAccessLogTagHasBeenSet(false)
{
}

bool CreateSessionRequest::HasEmbeddedError(Aws::IOStream &body,
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

Aws::String CreateSessionRequest::SerializePayload() const
{
  return {};
}

void CreateSessionRequest::AddQueryStringParameters(URI& uri) const
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

Aws::Http::HeaderValueCollection CreateSessionRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  Aws::StringStream ss;
  if(m_sessionModeHasBeenSet && m_sessionMode != SessionMode::NOT_SET)
  {
    headers.emplace("x-amz-create-session-mode", SessionModeMapper::GetNameForSessionMode(m_sessionMode));
  }

  if(m_serverSideEncryptionHasBeenSet && m_serverSideEncryption != ServerSideEncryption::NOT_SET)
  {
    headers.emplace("x-amz-server-side-encryption", ServerSideEncryptionMapper::GetNameForServerSideEncryption(m_serverSideEncryption));
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

  return headers;
}

CreateSessionRequest::EndpointParameters CreateSessionRequest::GetEndpointContextParams() const
{
    EndpointParameters parameters;
    // Static context parameters
    parameters.emplace_back(Aws::String("DisableS3ExpressSessionAuth"), true, Aws::Endpoint::EndpointParameter::ParameterOrigin::STATIC_CONTEXT);
    // Operation context parameters
    if (BucketHasBeenSet()) {
        parameters.emplace_back(Aws::String("Bucket"), this->GetBucket(), Aws::Endpoint::EndpointParameter::ParameterOrigin::OPERATION_CONTEXT);
    }
    return parameters;
}
