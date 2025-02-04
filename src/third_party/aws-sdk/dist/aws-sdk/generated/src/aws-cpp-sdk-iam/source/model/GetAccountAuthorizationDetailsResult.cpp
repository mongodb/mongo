/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetAccountAuthorizationDetailsResult.h>
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

GetAccountAuthorizationDetailsResult::GetAccountAuthorizationDetailsResult() : 
    m_isTruncated(false)
{
}

GetAccountAuthorizationDetailsResult::GetAccountAuthorizationDetailsResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : GetAccountAuthorizationDetailsResult()
{
  *this = result;
}

GetAccountAuthorizationDetailsResult& GetAccountAuthorizationDetailsResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "GetAccountAuthorizationDetailsResult"))
  {
    resultNode = rootNode.FirstChild("GetAccountAuthorizationDetailsResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode userDetailListNode = resultNode.FirstChild("UserDetailList");
    if(!userDetailListNode.IsNull())
    {
      XmlNode userDetailListMember = userDetailListNode.FirstChild("member");
      while(!userDetailListMember.IsNull())
      {
        m_userDetailList.push_back(userDetailListMember);
        userDetailListMember = userDetailListMember.NextNode("member");
      }

    }
    XmlNode groupDetailListNode = resultNode.FirstChild("GroupDetailList");
    if(!groupDetailListNode.IsNull())
    {
      XmlNode groupDetailListMember = groupDetailListNode.FirstChild("member");
      while(!groupDetailListMember.IsNull())
      {
        m_groupDetailList.push_back(groupDetailListMember);
        groupDetailListMember = groupDetailListMember.NextNode("member");
      }

    }
    XmlNode roleDetailListNode = resultNode.FirstChild("RoleDetailList");
    if(!roleDetailListNode.IsNull())
    {
      XmlNode roleDetailListMember = roleDetailListNode.FirstChild("member");
      while(!roleDetailListMember.IsNull())
      {
        m_roleDetailList.push_back(roleDetailListMember);
        roleDetailListMember = roleDetailListMember.NextNode("member");
      }

    }
    XmlNode policiesNode = resultNode.FirstChild("Policies");
    if(!policiesNode.IsNull())
    {
      XmlNode policiesMember = policiesNode.FirstChild("member");
      while(!policiesMember.IsNull())
      {
        m_policies.push_back(policiesMember);
        policiesMember = policiesMember.NextNode("member");
      }

    }
    XmlNode isTruncatedNode = resultNode.FirstChild("IsTruncated");
    if(!isTruncatedNode.IsNull())
    {
      m_isTruncated = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(isTruncatedNode.GetText()).c_str()).c_str());
    }
    XmlNode markerNode = resultNode.FirstChild("Marker");
    if(!markerNode.IsNull())
    {
      m_marker = Aws::Utils::Xml::DecodeEscapedXmlText(markerNode.GetText());
    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::GetAccountAuthorizationDetailsResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
