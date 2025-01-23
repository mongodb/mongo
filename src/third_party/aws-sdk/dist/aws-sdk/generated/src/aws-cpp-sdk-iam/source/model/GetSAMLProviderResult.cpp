/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetSAMLProviderResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/logging/LogMacros.h>

#include <utility>

using namespace Aws::IAM::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils::Logging;
using namespace Aws::Utils;
using namespace Aws;

GetSAMLProviderResult::GetSAMLProviderResult()
{
}

GetSAMLProviderResult::GetSAMLProviderResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

GetSAMLProviderResult& GetSAMLProviderResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "GetSAMLProviderResult"))
  {
    resultNode = rootNode.FirstChild("GetSAMLProviderResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode sAMLMetadataDocumentNode = resultNode.FirstChild("SAMLMetadataDocument");
    if(!sAMLMetadataDocumentNode.IsNull())
    {
      m_sAMLMetadataDocument = Aws::Utils::Xml::DecodeEscapedXmlText(sAMLMetadataDocumentNode.GetText());
    }
    XmlNode createDateNode = resultNode.FirstChild("CreateDate");
    if(!createDateNode.IsNull())
    {
      m_createDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(createDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
    }
    XmlNode validUntilNode = resultNode.FirstChild("ValidUntil");
    if(!validUntilNode.IsNull())
    {
      m_validUntil = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(validUntilNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
    }
    XmlNode tagsNode = resultNode.FirstChild("Tags");
    if(!tagsNode.IsNull())
    {
      XmlNode tagsMember = tagsNode.FirstChild("member");
      while(!tagsMember.IsNull())
      {
        m_tags.push_back(tagsMember);
        tagsMember = tagsMember.NextNode("member");
      }

    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::GetSAMLProviderResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
