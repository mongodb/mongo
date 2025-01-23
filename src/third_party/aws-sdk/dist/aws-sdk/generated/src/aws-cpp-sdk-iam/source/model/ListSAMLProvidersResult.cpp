/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListSAMLProvidersResult.h>
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

ListSAMLProvidersResult::ListSAMLProvidersResult()
{
}

ListSAMLProvidersResult::ListSAMLProvidersResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

ListSAMLProvidersResult& ListSAMLProvidersResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "ListSAMLProvidersResult"))
  {
    resultNode = rootNode.FirstChild("ListSAMLProvidersResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode sAMLProviderListNode = resultNode.FirstChild("SAMLProviderList");
    if(!sAMLProviderListNode.IsNull())
    {
      XmlNode sAMLProviderListMember = sAMLProviderListNode.FirstChild("member");
      while(!sAMLProviderListMember.IsNull())
      {
        m_sAMLProviderList.push_back(sAMLProviderListMember);
        sAMLProviderListMember = sAMLProviderListMember.NextNode("member");
      }

    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::ListSAMLProvidersResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
