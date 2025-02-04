/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetOpenIDConnectProviderResult.h>
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

GetOpenIDConnectProviderResult::GetOpenIDConnectProviderResult()
{
}

GetOpenIDConnectProviderResult::GetOpenIDConnectProviderResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

GetOpenIDConnectProviderResult& GetOpenIDConnectProviderResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "GetOpenIDConnectProviderResult"))
  {
    resultNode = rootNode.FirstChild("GetOpenIDConnectProviderResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode urlNode = resultNode.FirstChild("Url");
    if(!urlNode.IsNull())
    {
      m_url = Aws::Utils::Xml::DecodeEscapedXmlText(urlNode.GetText());
    }
    XmlNode clientIDListNode = resultNode.FirstChild("ClientIDList");
    if(!clientIDListNode.IsNull())
    {
      XmlNode clientIDListMember = clientIDListNode.FirstChild("member");
      while(!clientIDListMember.IsNull())
      {
        m_clientIDList.push_back(clientIDListMember.GetText());
        clientIDListMember = clientIDListMember.NextNode("member");
      }

    }
    XmlNode thumbprintListNode = resultNode.FirstChild("ThumbprintList");
    if(!thumbprintListNode.IsNull())
    {
      XmlNode thumbprintListMember = thumbprintListNode.FirstChild("member");
      while(!thumbprintListMember.IsNull())
      {
        m_thumbprintList.push_back(thumbprintListMember.GetText());
        thumbprintListMember = thumbprintListMember.NextNode("member");
      }

    }
    XmlNode createDateNode = resultNode.FirstChild("CreateDate");
    if(!createDateNode.IsNull())
    {
      m_createDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(createDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::GetOpenIDConnectProviderResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
