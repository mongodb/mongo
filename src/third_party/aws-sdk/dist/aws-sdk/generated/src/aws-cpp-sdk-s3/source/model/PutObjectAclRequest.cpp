/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/PutObjectAclRequest.h>
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

PutObjectAclRequest::PutObjectAclRequest() : 
    m_aCL(ObjectCannedACL::NOT_SET),
    m_aCLHasBeenSet(false),
    m_accessControlPolicyHasBeenSet(false),
    m_bucketHasBeenSet(false),
    m_contentMD5HasBeenSet(false),
    m_checksumAlgorithm(ChecksumAlgorithm::NOT_SET),
    m_checksumAlgorithmHasBeenSet(false),
    m_grantFullControlHasBeenSet(false),
    m_grantReadHasBeenSet(false),
    m_grantReadACPHasBeenSet(false),
    m_grantWriteHasBeenSet(false),
    m_grantWriteACPHasBeenSet(false),
    m_keyHasBeenSet(false),
    m_requestPayer(RequestPayer::NOT_SET),
    m_requestPayerHasBeenSet(false),
    m_versionIdHasBeenSet(false),
    m_expectedBucketOwnerHasBeenSet(false),
    m_customizedAccessLogTagHasBeenSet(false)
{
}

bool PutObjectAclRequest::HasEmbeddedError(Aws::IOStream &body,
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

Aws::String PutObjectAclRequest::SerializePayload() const
{
  XmlDocument payloadDoc = XmlDocument::CreateWithRootNode("AccessControlPolicy");

  XmlNode parentNode = payloadDoc.GetRootElement();
  parentNode.SetAttributeValue("xmlns", "http://s3.amazonaws.com/doc/2006-03-01/");

  m_accessControlPolicy.AddToNode(parentNode);
  if(parentNode.HasChildren())
  {
    return payloadDoc.ConvertToString();
  }

  return {};
}

void PutObjectAclRequest::AddQueryStringParameters(URI& uri) const
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

Aws::Http::HeaderValueCollection PutObjectAclRequest::GetRequestSpecificHeaders() const
{
  Aws::Http::HeaderValueCollection headers;
  Aws::StringStream ss;
  if(m_aCLHasBeenSet && m_aCL != ObjectCannedACL::NOT_SET)
  {
    headers.emplace("x-amz-acl", ObjectCannedACLMapper::GetNameForObjectCannedACL(m_aCL));
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

  if(m_grantWriteHasBeenSet)
  {
    ss << m_grantWrite;
    headers.emplace("x-amz-grant-write",  ss.str());
    ss.str("");
  }

  if(m_grantWriteACPHasBeenSet)
  {
    ss << m_grantWriteACP;
    headers.emplace("x-amz-grant-write-acp",  ss.str());
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

PutObjectAclRequest::EndpointParameters PutObjectAclRequest::GetEndpointContextParams() const
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

Aws::String PutObjectAclRequest::GetChecksumAlgorithmName() const
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

