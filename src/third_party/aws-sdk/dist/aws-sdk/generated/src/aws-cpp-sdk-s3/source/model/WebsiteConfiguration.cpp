/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/WebsiteConfiguration.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Xml;
using namespace Aws::Utils;

namespace Aws
{
namespace S3
{
namespace Model
{

WebsiteConfiguration::WebsiteConfiguration() : 
    m_errorDocumentHasBeenSet(false),
    m_indexDocumentHasBeenSet(false),
    m_redirectAllRequestsToHasBeenSet(false),
    m_routingRulesHasBeenSet(false)
{
}

WebsiteConfiguration::WebsiteConfiguration(const XmlNode& xmlNode)
  : WebsiteConfiguration()
{
  *this = xmlNode;
}

WebsiteConfiguration& WebsiteConfiguration::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode errorDocumentNode = resultNode.FirstChild("ErrorDocument");
    if(!errorDocumentNode.IsNull())
    {
      m_errorDocument = errorDocumentNode;
      m_errorDocumentHasBeenSet = true;
    }
    XmlNode indexDocumentNode = resultNode.FirstChild("IndexDocument");
    if(!indexDocumentNode.IsNull())
    {
      m_indexDocument = indexDocumentNode;
      m_indexDocumentHasBeenSet = true;
    }
    XmlNode redirectAllRequestsToNode = resultNode.FirstChild("RedirectAllRequestsTo");
    if(!redirectAllRequestsToNode.IsNull())
    {
      m_redirectAllRequestsTo = redirectAllRequestsToNode;
      m_redirectAllRequestsToHasBeenSet = true;
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

      m_routingRulesHasBeenSet = true;
    }
  }

  return *this;
}

void WebsiteConfiguration::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_errorDocumentHasBeenSet)
  {
   XmlNode errorDocumentNode = parentNode.CreateChildElement("ErrorDocument");
   m_errorDocument.AddToNode(errorDocumentNode);
  }

  if(m_indexDocumentHasBeenSet)
  {
   XmlNode indexDocumentNode = parentNode.CreateChildElement("IndexDocument");
   m_indexDocument.AddToNode(indexDocumentNode);
  }

  if(m_redirectAllRequestsToHasBeenSet)
  {
   XmlNode redirectAllRequestsToNode = parentNode.CreateChildElement("RedirectAllRequestsTo");
   m_redirectAllRequestsTo.AddToNode(redirectAllRequestsToNode);
  }

  if(m_routingRulesHasBeenSet)
  {
   XmlNode routingRulesParentNode = parentNode.CreateChildElement("RoutingRules");
   for(const auto& item : m_routingRules)
   {
     XmlNode routingRulesNode = routingRulesParentNode.CreateChildElement("RoutingRule");
     item.AddToNode(routingRulesNode);
   }
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
