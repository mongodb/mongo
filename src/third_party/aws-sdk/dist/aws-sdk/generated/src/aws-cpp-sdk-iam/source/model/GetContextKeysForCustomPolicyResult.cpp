/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetContextKeysForCustomPolicyResult.h>
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

GetContextKeysForCustomPolicyResult::GetContextKeysForCustomPolicyResult()
{
}

GetContextKeysForCustomPolicyResult::GetContextKeysForCustomPolicyResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

GetContextKeysForCustomPolicyResult& GetContextKeysForCustomPolicyResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "GetContextKeysForCustomPolicyResult"))
  {
    resultNode = rootNode.FirstChild("GetContextKeysForCustomPolicyResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode contextKeyNamesNode = resultNode.FirstChild("ContextKeyNames");
    if(!contextKeyNamesNode.IsNull())
    {
      XmlNode contextKeyNamesMember = contextKeyNamesNode.FirstChild("member");
      while(!contextKeyNamesMember.IsNull())
      {
        m_contextKeyNames.push_back(contextKeyNamesMember.GetText());
        contextKeyNamesMember = contextKeyNamesMember.NextNode("member");
      }

    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::GetContextKeysForCustomPolicyResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
