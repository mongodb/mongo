/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/LifecycleRuleFilter.h>
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

LifecycleRuleFilter::LifecycleRuleFilter() : 
    m_prefixHasBeenSet(false),
    m_tagHasBeenSet(false),
    m_objectSizeGreaterThan(0),
    m_objectSizeGreaterThanHasBeenSet(false),
    m_objectSizeLessThan(0),
    m_objectSizeLessThanHasBeenSet(false),
    m_andHasBeenSet(false)
{
}

LifecycleRuleFilter::LifecycleRuleFilter(const XmlNode& xmlNode)
  : LifecycleRuleFilter()
{
  *this = xmlNode;
}

LifecycleRuleFilter& LifecycleRuleFilter::operator =(const XmlNode& xmlNode)
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
    XmlNode tagNode = resultNode.FirstChild("Tag");
    if(!tagNode.IsNull())
    {
      m_tag = tagNode;
      m_tagHasBeenSet = true;
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
    XmlNode andNode = resultNode.FirstChild("And");
    if(!andNode.IsNull())
    {
      m_and = andNode;
      m_andHasBeenSet = true;
    }
  }

  return *this;
}

void LifecycleRuleFilter::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_prefixHasBeenSet)
  {
   XmlNode prefixNode = parentNode.CreateChildElement("Prefix");
   prefixNode.SetText(m_prefix);
  }

  if(m_tagHasBeenSet)
  {
   XmlNode tagNode = parentNode.CreateChildElement("Tag");
   m_tag.AddToNode(tagNode);
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

  if(m_andHasBeenSet)
  {
   XmlNode andNode = parentNode.CreateChildElement("And");
   m_and.AddToNode(andNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
