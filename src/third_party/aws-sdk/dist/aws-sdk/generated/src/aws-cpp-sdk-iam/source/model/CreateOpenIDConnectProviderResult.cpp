/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/CreateOpenIDConnectProviderResult.h>
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

CreateOpenIDConnectProviderResult::CreateOpenIDConnectProviderResult()
{
}

CreateOpenIDConnectProviderResult::CreateOpenIDConnectProviderResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

CreateOpenIDConnectProviderResult& CreateOpenIDConnectProviderResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "CreateOpenIDConnectProviderResult"))
  {
    resultNode = rootNode.FirstChild("CreateOpenIDConnectProviderResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode openIDConnectProviderArnNode = resultNode.FirstChild("OpenIDConnectProviderArn");
    if(!openIDConnectProviderArnNode.IsNull())
    {
      m_openIDConnectProviderArn = Aws::Utils::Xml::DecodeEscapedXmlText(openIDConnectProviderArnNode.GetText());
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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::CreateOpenIDConnectProviderResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
