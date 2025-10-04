/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/GetObjectAttributesParts.h>
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

GetObjectAttributesParts::GetObjectAttributesParts() : 
    m_totalPartsCount(0),
    m_totalPartsCountHasBeenSet(false),
    m_partNumberMarker(0),
    m_partNumberMarkerHasBeenSet(false),
    m_nextPartNumberMarker(0),
    m_nextPartNumberMarkerHasBeenSet(false),
    m_maxParts(0),
    m_maxPartsHasBeenSet(false),
    m_isTruncated(false),
    m_isTruncatedHasBeenSet(false),
    m_partsHasBeenSet(false)
{
}

GetObjectAttributesParts::GetObjectAttributesParts(const XmlNode& xmlNode)
  : GetObjectAttributesParts()
{
  *this = xmlNode;
}

GetObjectAttributesParts& GetObjectAttributesParts::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode totalPartsCountNode = resultNode.FirstChild("PartsCount");
    if(!totalPartsCountNode.IsNull())
    {
      m_totalPartsCount = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(totalPartsCountNode.GetText()).c_str()).c_str());
      m_totalPartsCountHasBeenSet = true;
    }
    XmlNode partNumberMarkerNode = resultNode.FirstChild("PartNumberMarker");
    if(!partNumberMarkerNode.IsNull())
    {
      m_partNumberMarker = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(partNumberMarkerNode.GetText()).c_str()).c_str());
      m_partNumberMarkerHasBeenSet = true;
    }
    XmlNode nextPartNumberMarkerNode = resultNode.FirstChild("NextPartNumberMarker");
    if(!nextPartNumberMarkerNode.IsNull())
    {
      m_nextPartNumberMarker = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(nextPartNumberMarkerNode.GetText()).c_str()).c_str());
      m_nextPartNumberMarkerHasBeenSet = true;
    }
    XmlNode maxPartsNode = resultNode.FirstChild("MaxParts");
    if(!maxPartsNode.IsNull())
    {
      m_maxParts = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(maxPartsNode.GetText()).c_str()).c_str());
      m_maxPartsHasBeenSet = true;
    }
    XmlNode isTruncatedNode = resultNode.FirstChild("IsTruncated");
    if(!isTruncatedNode.IsNull())
    {
      m_isTruncated = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(isTruncatedNode.GetText()).c_str()).c_str());
      m_isTruncatedHasBeenSet = true;
    }
    XmlNode partsNode = resultNode.FirstChild("Part");
    if(!partsNode.IsNull())
    {
      XmlNode partMember = partsNode;
      while(!partMember.IsNull())
      {
        m_parts.push_back(partMember);
        partMember = partMember.NextNode("Part");
      }

      m_partsHasBeenSet = true;
    }
  }

  return *this;
}

void GetObjectAttributesParts::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_totalPartsCountHasBeenSet)
  {
   XmlNode totalPartsCountNode = parentNode.CreateChildElement("PartsCount");
   ss << m_totalPartsCount;
   totalPartsCountNode.SetText(ss.str());
   ss.str("");
  }

  if(m_partNumberMarkerHasBeenSet)
  {
   XmlNode partNumberMarkerNode = parentNode.CreateChildElement("PartNumberMarker");
   ss << m_partNumberMarker;
   partNumberMarkerNode.SetText(ss.str());
   ss.str("");
  }

  if(m_nextPartNumberMarkerHasBeenSet)
  {
   XmlNode nextPartNumberMarkerNode = parentNode.CreateChildElement("NextPartNumberMarker");
   ss << m_nextPartNumberMarker;
   nextPartNumberMarkerNode.SetText(ss.str());
   ss.str("");
  }

  if(m_maxPartsHasBeenSet)
  {
   XmlNode maxPartsNode = parentNode.CreateChildElement("MaxParts");
   ss << m_maxParts;
   maxPartsNode.SetText(ss.str());
   ss.str("");
  }

  if(m_isTruncatedHasBeenSet)
  {
   XmlNode isTruncatedNode = parentNode.CreateChildElement("IsTruncated");
   ss << std::boolalpha << m_isTruncated;
   isTruncatedNode.SetText(ss.str());
   ss.str("");
  }

  if(m_partsHasBeenSet)
  {
   for(const auto& item : m_parts)
   {
     XmlNode partsNode = parentNode.CreateChildElement("Part");
     item.AddToNode(partsNode);
   }
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
