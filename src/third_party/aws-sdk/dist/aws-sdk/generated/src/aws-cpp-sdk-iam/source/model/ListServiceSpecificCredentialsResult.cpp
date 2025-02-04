/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListServiceSpecificCredentialsResult.h>
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

ListServiceSpecificCredentialsResult::ListServiceSpecificCredentialsResult()
{
}

ListServiceSpecificCredentialsResult::ListServiceSpecificCredentialsResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

ListServiceSpecificCredentialsResult& ListServiceSpecificCredentialsResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "ListServiceSpecificCredentialsResult"))
  {
    resultNode = rootNode.FirstChild("ListServiceSpecificCredentialsResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode serviceSpecificCredentialsNode = resultNode.FirstChild("ServiceSpecificCredentials");
    if(!serviceSpecificCredentialsNode.IsNull())
    {
      XmlNode serviceSpecificCredentialsMember = serviceSpecificCredentialsNode.FirstChild("member");
      while(!serviceSpecificCredentialsMember.IsNull())
      {
        m_serviceSpecificCredentials.push_back(serviceSpecificCredentialsMember);
        serviceSpecificCredentialsMember = serviceSpecificCredentialsMember.NextNode("member");
      }

    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::ListServiceSpecificCredentialsResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
