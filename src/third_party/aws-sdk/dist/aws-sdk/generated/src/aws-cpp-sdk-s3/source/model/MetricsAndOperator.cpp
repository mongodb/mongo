/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/MetricsAndOperator.h>
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

MetricsAndOperator::MetricsAndOperator() : 
    m_prefixHasBeenSet(false),
    m_tagsHasBeenSet(false),
    m_accessPointArnHasBeenSet(false)
{
}

MetricsAndOperator::MetricsAndOperator(const XmlNode& xmlNode)
  : MetricsAndOperator()
{
  *this = xmlNode;
}

MetricsAndOperator& MetricsAndOperator::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode prefixNode = resultNode.FirstChild("Prefix");
    if(!prefixNode.IsNull())
    {
      m_prefix = Aws::Utils::Xml::DecodeEscapedXmlText(prefixNode.GetText());
      m_prefixHasBeenSet = true;
    }
    XmlNode tagsNode = resultNode.FirstChild("Tag");
    if(!tagsNode.IsNull())
    {
      XmlNode tagMember = tagsNode;
      while(!tagMember.IsNull())
      {
        m_tags.push_back(tagMember);
        tagMember = tagMember.NextNode("Tag");
      }

      m_tagsHasBeenSet = true;
    }
    XmlNode accessPointArnNode = resultNode.FirstChild("AccessPointArn");
    if(!accessPointArnNode.IsNull())
    {
      m_accessPointArn = Aws::Utils::Xml::DecodeEscapedXmlText(accessPointArnNode.GetText());
      m_accessPointArnHasBeenSet = true;
    }
  }

  return *this;
}

void MetricsAndOperator::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_prefixHasBeenSet)
  {
   XmlNode prefixNode = parentNode.CreateChildElement("Prefix");
   prefixNode.SetText(m_prefix);
  }

  if(m_tagsHasBeenSet)
  {
   for(const auto& item : m_tags)
   {
     XmlNode tagsNode = parentNode.CreateChildElement("Tag");
     item.AddToNode(tagsNode);
   }
  }

  if(m_accessPointArnHasBeenSet)
  {
   XmlNode accessPointArnNode = parentNode.CreateChildElement("AccessPointArn");
   accessPointArnNode.SetText(m_accessPointArn);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
