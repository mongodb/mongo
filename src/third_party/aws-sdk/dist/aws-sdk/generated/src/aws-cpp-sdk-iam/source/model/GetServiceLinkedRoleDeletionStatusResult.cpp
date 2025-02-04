/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetServiceLinkedRoleDeletionStatusResult.h>
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

GetServiceLinkedRoleDeletionStatusResult::GetServiceLinkedRoleDeletionStatusResult() : 
    m_status(DeletionTaskStatusType::NOT_SET)
{
}

GetServiceLinkedRoleDeletionStatusResult::GetServiceLinkedRoleDeletionStatusResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : GetServiceLinkedRoleDeletionStatusResult()
{
  *this = result;
}

GetServiceLinkedRoleDeletionStatusResult& GetServiceLinkedRoleDeletionStatusResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "GetServiceLinkedRoleDeletionStatusResult"))
  {
    resultNode = rootNode.FirstChild("GetServiceLinkedRoleDeletionStatusResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = DeletionTaskStatusTypeMapper::GetDeletionTaskStatusTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
    }
    XmlNode reasonNode = resultNode.FirstChild("Reason");
    if(!reasonNode.IsNull())
    {
      m_reason = reasonNode;
    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::GetServiceLinkedRoleDeletionStatusResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
