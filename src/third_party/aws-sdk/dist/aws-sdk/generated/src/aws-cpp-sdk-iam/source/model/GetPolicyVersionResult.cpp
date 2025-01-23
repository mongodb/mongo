/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GetPolicyVersionResult.h>
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

GetPolicyVersionResult::GetPolicyVersionResult()
{
}

GetPolicyVersionResult::GetPolicyVersionResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

GetPolicyVersionResult& GetPolicyVersionResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "GetPolicyVersionResult"))
  {
    resultNode = rootNode.FirstChild("GetPolicyVersionResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode policyVersionNode = resultNode.FirstChild("PolicyVersion");
    if(!policyVersionNode.IsNull())
    {
      m_policyVersion = policyVersionNode;
    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::GetPolicyVersionResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
