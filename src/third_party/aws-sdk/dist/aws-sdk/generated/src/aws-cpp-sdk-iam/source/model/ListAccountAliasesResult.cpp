/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListAccountAliasesResult.h>
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

ListAccountAliasesResult::ListAccountAliasesResult() : 
    m_isTruncated(false)
{
}

ListAccountAliasesResult::ListAccountAliasesResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : ListAccountAliasesResult()
{
  *this = result;
}

ListAccountAliasesResult& ListAccountAliasesResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "ListAccountAliasesResult"))
  {
    resultNode = rootNode.FirstChild("ListAccountAliasesResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode accountAliasesNode = resultNode.FirstChild("AccountAliases");
    if(!accountAliasesNode.IsNull())
    {
      XmlNode accountAliasesMember = accountAliasesNode.FirstChild("member");
      while(!accountAliasesMember.IsNull())
      {
        m_accountAliases.push_back(accountAliasesMember.GetText());
        accountAliasesMember = accountAliasesMember.NextNode("member");
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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::ListAccountAliasesResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
