/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/RoutingRule.h>
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

RoutingRule::RoutingRule() : 
    m_conditionHasBeenSet(false),
    m_redirectHasBeenSet(false)
{
}

RoutingRule::RoutingRule(const XmlNode& xmlNode)
  : RoutingRule()
{
  *this = xmlNode;
}

RoutingRule& RoutingRule::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode conditionNode = resultNode.FirstChild("Condition");
    if(!conditionNode.IsNull())
    {
      m_condition = conditionNode;
      m_conditionHasBeenSet = true;
    }
    XmlNode redirectNode = resultNode.FirstChild("Redirect");
    if(!redirectNode.IsNull())
    {
      m_redirect = redirectNode;
      m_redirectHasBeenSet = true;
    }
  }

  return *this;
}

void RoutingRule::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_conditionHasBeenSet)
  {
   XmlNode conditionNode = parentNode.CreateChildElement("Condition");
   m_condition.AddToNode(conditionNode);
  }

  if(m_redirectHasBeenSet)
  {
   XmlNode redirectNode = parentNode.CreateChildElement("Redirect");
   m_redirect.AddToNode(redirectNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
