/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/S3KeyFilter.h>
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

S3KeyFilter::S3KeyFilter() : 
    m_filterRulesHasBeenSet(false)
{
}

S3KeyFilter::S3KeyFilter(const XmlNode& xmlNode)
  : S3KeyFilter()
{
  *this = xmlNode;
}

S3KeyFilter& S3KeyFilter::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode filterRulesNode = resultNode.FirstChild("FilterRule");
    if(!filterRulesNode.IsNull())
    {
      XmlNode filterRuleMember = filterRulesNode;
      while(!filterRuleMember.IsNull())
      {
        m_filterRules.push_back(filterRuleMember);
        filterRuleMember = filterRuleMember.NextNode("FilterRule");
      }

      m_filterRulesHasBeenSet = true;
    }
  }

  return *this;
}

void S3KeyFilter::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_filterRulesHasBeenSet)
  {
   for(const auto& item : m_filterRules)
   {
     XmlNode filterRulesNode = parentNode.CreateChildElement("FilterRule");
     item.AddToNode(filterRulesNode);
   }
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
