/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateSAMLProviderResult.h>
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

CreateSAMLProviderResult::CreateSAMLProviderResult()
{
}

CreateSAMLProviderResult::CreateSAMLProviderResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

CreateSAMLProviderResult& CreateSAMLProviderResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "CreateSAMLProviderResult"))
  {
    resultNode = rootNode.FirstChild("CreateSAMLProviderResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode sAMLProviderArnNode = resultNode.FirstChild("SAMLProviderArn");
    if(!sAMLProviderArnNode.IsNull())
    {
      m_sAMLProviderArn = Aws::Utils::Xml::DecodeEscapedXmlText(sAMLProviderArnNode.GetText());
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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::CreateSAMLProviderResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
