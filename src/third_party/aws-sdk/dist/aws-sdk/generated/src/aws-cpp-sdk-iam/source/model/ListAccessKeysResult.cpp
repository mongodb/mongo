/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListAccessKeysResult.h>
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

ListAccessKeysResult::ListAccessKeysResult() : 
    m_isTruncated(false)
{
}

ListAccessKeysResult::ListAccessKeysResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : ListAccessKeysResult()
{
  *this = result;
}

ListAccessKeysResult& ListAccessKeysResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "ListAccessKeysResult"))
  {
    resultNode = rootNode.FirstChild("ListAccessKeysResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode accessKeyMetadataNode = resultNode.FirstChild("AccessKeyMetadata");
    if(!accessKeyMetadataNode.IsNull())
    {
      XmlNode accessKeyMetadataMember = accessKeyMetadataNode.FirstChild("member");
      while(!accessKeyMetadataMember.IsNull())
      {
        m_accessKeyMetadata.push_back(accessKeyMetadataMember);
        accessKeyMetadataMember = accessKeyMetadataMember.NextNode("member");
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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::ListAccessKeysResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
