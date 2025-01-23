/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListPolicyVersionsResult.h>
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

ListPolicyVersionsResult::ListPolicyVersionsResult() : 
    m_isTruncated(false)
{
}

ListPolicyVersionsResult::ListPolicyVersionsResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : ListPolicyVersionsResult()
{
  *this = result;
}

ListPolicyVersionsResult& ListPolicyVersionsResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "ListPolicyVersionsResult"))
  {
    resultNode = rootNode.FirstChild("ListPolicyVersionsResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode versionsNode = resultNode.FirstChild("Versions");
    if(!versionsNode.IsNull())
    {
      XmlNode versionsMember = versionsNode.FirstChild("member");
      while(!versionsMember.IsNull())
      {
        m_versions.push_back(versionsMember);
        versionsMember = versionsMember.NextNode("member");
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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::ListPolicyVersionsResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
