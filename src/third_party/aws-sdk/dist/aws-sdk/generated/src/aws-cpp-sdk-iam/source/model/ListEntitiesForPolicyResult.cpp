/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListEntitiesForPolicyResult.h>
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

ListEntitiesForPolicyResult::ListEntitiesForPolicyResult() : 
    m_isTruncated(false)
{
}

ListEntitiesForPolicyResult::ListEntitiesForPolicyResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : ListEntitiesForPolicyResult()
{
  *this = result;
}

ListEntitiesForPolicyResult& ListEntitiesForPolicyResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "ListEntitiesForPolicyResult"))
  {
    resultNode = rootNode.FirstChild("ListEntitiesForPolicyResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode policyGroupsNode = resultNode.FirstChild("PolicyGroups");
    if(!policyGroupsNode.IsNull())
    {
      XmlNode policyGroupsMember = policyGroupsNode.FirstChild("member");
      while(!policyGroupsMember.IsNull())
      {
        m_policyGroups.push_back(policyGroupsMember);
        policyGroupsMember = policyGroupsMember.NextNode("member");
      }

    }
    XmlNode policyUsersNode = resultNode.FirstChild("PolicyUsers");
    if(!policyUsersNode.IsNull())
    {
      XmlNode policyUsersMember = policyUsersNode.FirstChild("member");
      while(!policyUsersMember.IsNull())
      {
        m_policyUsers.push_back(policyUsersMember);
        policyUsersMember = policyUsersMember.NextNode("member");
      }

    }
    XmlNode policyRolesNode = resultNode.FirstChild("PolicyRoles");
    if(!policyRolesNode.IsNull())
    {
      XmlNode policyRolesMember = policyRolesNode.FirstChild("member");
      while(!policyRolesMember.IsNull())
      {
        m_policyRoles.push_back(policyRolesMember);
        policyRolesMember = policyRolesMember.NextNode("member");
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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::ListEntitiesForPolicyResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
