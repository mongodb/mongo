/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/LifecycleRuleAndOperator.h>
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

LifecycleRuleAndOperator::LifecycleRuleAndOperator() : 
    m_prefixHasBeenSet(false),
    m_tagsHasBeenSet(false),
    m_objectSizeGreaterThan(0),
    m_objectSizeGreaterThanHasBeenSet(false),
    m_objectSizeLessThan(0),
    m_objectSizeLessThanHasBeenSet(false)
{
}

LifecycleRuleAndOperator::LifecycleRuleAndOperator(const XmlNode& xmlNode)
  : LifecycleRuleAndOperator()
{
  *this = xmlNode;
}

LifecycleRuleAndOperator& LifecycleRuleAndOperator::operator =(const XmlNode& xmlNode)
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
    XmlNode objectSizeGreaterThanNode = resultNode.FirstChild("ObjectSizeGreaterThan");
    if(!objectSizeGreaterThanNode.IsNull())
    {
      m_objectSizeGreaterThan = StringUtils::ConvertToInt64(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(objectSizeGreaterThanNode.GetText()).c_str()).c_str());
      m_objectSizeGreaterThanHasBeenSet = true;
    }
    XmlNode objectSizeLessThanNode = resultNode.FirstChild("ObjectSizeLessThan");
    if(!objectSizeLessThanNode.IsNull())
    {
      m_objectSizeLessThan = StringUtils::ConvertToInt64(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(objectSizeLessThanNode.GetText()).c_str()).c_str());
      m_objectSizeLessThanHasBeenSet = true;
    }
  }

  return *this;
}

void LifecycleRuleAndOperator::AddToNode(XmlNode& parentNode) const
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

  if(m_objectSizeGreaterThanHasBeenSet)
  {
   XmlNode objectSizeGreaterThanNode = parentNode.CreateChildElement("ObjectSizeGreaterThan");
   ss << m_objectSizeGreaterThan;
   objectSizeGreaterThanNode.SetText(ss.str());
   ss.str("");
  }

  if(m_objectSizeLessThanHasBeenSet)
  {
   XmlNode objectSizeLessThanNode = parentNode.CreateChildElement("ObjectSizeLessThan");
   ss << m_objectSizeLessThan;
   objectSizeLessThanNode.SetText(ss.str());
   ss.str("");
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
