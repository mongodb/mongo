/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/SimulatePrincipalPolicyResult.h>
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

SimulatePrincipalPolicyResult::SimulatePrincipalPolicyResult() : 
    m_isTruncated(false)
{
}

SimulatePrincipalPolicyResult::SimulatePrincipalPolicyResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
  : SimulatePrincipalPolicyResult()
{
  *this = result;
}

SimulatePrincipalPolicyResult& SimulatePrincipalPolicyResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "SimulatePrincipalPolicyResult"))
  {
    resultNode = rootNode.FirstChild("SimulatePrincipalPolicyResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode evaluationResultsNode = resultNode.FirstChild("EvaluationResults");
    if(!evaluationResultsNode.IsNull())
    {
      XmlNode evaluationResultsMember = evaluationResultsNode.FirstChild("member");
      while(!evaluationResultsMember.IsNull())
      {
        m_evaluationResults.push_back(evaluationResultsMember);
        evaluationResultsMember = evaluationResultsMember.NextNode("member");
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
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::SimulatePrincipalPolicyResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
