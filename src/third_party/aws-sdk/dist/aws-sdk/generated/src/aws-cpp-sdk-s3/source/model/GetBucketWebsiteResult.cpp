/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/GetBucketWebsiteResult.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::S3::Model;
using namespace Aws::Utils::Xml;
using namespace Aws::Utils;
using namespace Aws;

GetBucketWebsiteResult::GetBucketWebsiteResult()
{
}

GetBucketWebsiteResult::GetBucketWebsiteResult(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  *this = result;
}

GetBucketWebsiteResult& GetBucketWebsiteResult::operator =(const Aws::AmazonWebServiceResult<XmlDocument>& result)
{
  const XmlDocument& xmlDocument = result.GetPayload();
  XmlNode resultNode = xmlDocument.GetRootElement();

  if(!resultNode.IsNull())
  {
    XmlNode redirectAllRequestsToNode = resultNode.FirstChild("RedirectAllRequestsTo");
    if(!redirectAllRequestsToNode.IsNull())
    {
      m_redirectAllRequestsTo = redirectAllRequestsToNode;
    }
    XmlNode indexDocumentNode = resultNode.FirstChild("IndexDocument");
    if(!indexDocumentNode.IsNull())
    {
      m_indexDocument = indexDocumentNode;
    }
    XmlNode errorDocumentNode = resultNode.FirstChild("ErrorDocument");
    if(!errorDocumentNode.IsNull())
    {
      m_errorDocument = errorDocumentNode;
    }
    XmlNode routingRulesNode = resultNode.FirstChild("RoutingRules");
    if(!routingRulesNode.IsNull())
    {
      XmlNode routingRulesMember = routingRulesNode.FirstChild("RoutingRule");
      while(!routingRulesMember.IsNull())
      {
        m_routingRules.push_back(routingRulesMember);
        routingRulesMember = routingRulesMember.NextNode("RoutingRule");
      }

    }
  }

  const auto& headers = result.GetHeaderValueCollection();
  const auto& requestIdIter = headers.find("x-amz-request-id");
  if(requestIdIter != headers.end())
  {
    m_requestId = requestIdIter->second;
  }

  return *this;
}
