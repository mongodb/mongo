/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListAttachedRolePoliciesResult.h>
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

ListAttachedRolePoliciesResult::ListAttachedRolePoliciesResult() : 
    m_isTruncated(false)
{
}

ListAttachedRolePoliciesResult::ListAttachedRolePoliciesResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : ListAttachedRolePoliciesResult()
{
  *this = result;
}

ListAttachedRolePoliciesResult& ListAttachedRolePoliciesResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "ListAttachedRolePoliciesResult"))
  {
    resultNode = rootNode.FirstChild("ListAttachedRolePoliciesResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode attachedPoliciesNode = resultNode.FirstChild("AttachedPolicies");
    if(!attachedPoliciesNode.IsNull())
    {
      XmlNode attachedPoliciesMember = attachedPoliciesNode.FirstChild("member");
      while(!attachedPoliciesMember.IsNull())
      {
        m_attachedPolicies.push_back(attachedPoliciesMember);
        attachedPoliciesMember = attachedPoliciesMember.NextNode("member");
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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::ListAttachedRolePoliciesResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
