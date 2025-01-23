/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/DeleteServiceLinkedRoleResult.h>
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

DeleteServiceLinkedRoleResult::DeleteServiceLinkedRoleResult()
{
}

DeleteServiceLinkedRoleResult::DeleteServiceLinkedRoleResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

DeleteServiceLinkedRoleResult& DeleteServiceLinkedRoleResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "DeleteServiceLinkedRoleResult"))
  {
    resultNode = rootNode.FirstChild("DeleteServiceLinkedRoleResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode deletionTaskIdNode = resultNode.FirstChild("DeletionTaskId");
    if(!deletionTaskIdNode.IsNull())
    {
      m_deletionTaskId = Aws::Utils::Xml::DecodeEscapedXmlText(deletionTaskIdNode.GetText());
    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::DeleteServiceLinkedRoleResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
