/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ListOpenIDConnectProvidersResult.h>
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

ListOpenIDConnectProvidersResult::ListOpenIDConnectProvidersResult()
{
}

ListOpenIDConnectProvidersResult::ListOpenIDConnectProvidersResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

ListOpenIDConnectProvidersResult& ListOpenIDConnectProvidersResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode rootNode = xmlDocument.GetRootElement();
  XmlNode resultNode = rootNode;
  if (!rootNode.IsNull() && (rootNode.GetName() != "ListOpenIDConnectProvidersResult"))
  {
    resultNode = rootNode.FirstChild("ListOpenIDConnectProvidersResult");
  }

  if(!resultNode.IsNull())
  {
    XmlNode openIDConnectProviderListNode = resultNode.FirstChild("OpenIDConnectProviderList");
    if(!openIDConnectProviderListNode.IsNull())
    {
      XmlNode openIDConnectProviderListMember = openIDConnectProviderListNode.FirstChild("member");
      while(!openIDConnectProviderListMember.IsNull())
      {
        m_openIDConnectProviderList.push_back(openIDConnectProviderListMember);
        openIDConnectProviderListMember = openIDConnectProviderListMember.NextNode("member");
      }

    }
  }

  if (!rootNode.IsNull()) {
    XmlNode responseMetadataNode = rootNode.FirstChild("ResponseMetadata");
    m_responseMetadata = responseMetadataNode;
    AWS_LOGSTREAM_DEBUG("Aws::IAM::Model::ListOpenIDConnectProvidersResult", "x-amzn-request-id: " << m_responseMetadata.GetRequestId() );
  }
  return *this;
}
