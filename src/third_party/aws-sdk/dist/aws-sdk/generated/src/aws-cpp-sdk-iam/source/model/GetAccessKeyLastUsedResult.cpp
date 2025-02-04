/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetAccessKeyLastUsedResult.h>
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

GetAccessKeyLastUsedResult::GetAccessKeyLastUsedResult()
{
}

GetAccessKeyLastUsedResult::GetAccessKeyLastUsedResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

GetAccessKeyLastUsedResult& GetAccessKeyLastUsedResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "GetAccessKeyLastUsedResult"))
  {
    resultNode = rootNode.FirstChild("GetAccessKeyLastUsedResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode userNameNode = resultNode.FirstChild("UserName");
    if(!userNameNode.IsNull())
    {
      m_userName = Aws::Utils::Xml::DecodeEscapedXmlText(userNameNode.GetText());
    }
    XmlNode accessKeyLastUsedNode = resultNode.FirstChild("AccessKeyLastUsed");
    if(!accessKeyLastUsedNode.IsNull())
    {
      m_accessKeyLastUsed = accessKeyLastUsedNode;
    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::GetAccessKeyLastUsedResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
