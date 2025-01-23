/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListRolesResult.h>
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

ListRolesResult::ListRolesResult() : 
    m_isTruncated(false)
{
}

ListRolesResult::ListRolesResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : ListRolesResult()
{
  *this = result;
}

ListRolesResult& ListRolesResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "ListRolesResult"))
  {
    resultNode = rootNode.FirstChild("ListRolesResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode rolesNode = resultNode.FirstChild("Roles");
    if(!rolesNode.IsNull())
    {
      XmlNode rolesMember = rolesNode.FirstChild("member");
      while(!rolesMember.IsNull())
      {
        m_roles.push_back(rolesMember);
        rolesMember = rolesMember.NextNode("member");
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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::ListRolesResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
