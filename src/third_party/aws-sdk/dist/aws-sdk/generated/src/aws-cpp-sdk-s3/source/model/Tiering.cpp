/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Tiering.h>
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

Tiering::Tiering() : 
    m_days(0),
    m_daysHasBeenSet(false),
    m_accessTier(IntelligentTieringAccessTier::NOT_SET),
    m_accessTierHasBeenSet(false)
{
}

Tiering::Tiering(const XmlNode& xmlNode)
  : Tiering()
{
  *this = xmlNode;
}

Tiering& Tiering::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode daysNode = resultNode.FirstChild("Days");
    if(!daysNode.IsNull())
    {
      m_days = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(daysNode.GetText()).c_str()).c_str());
      m_daysHasBeenSet = true;
    }
    XmlNode accessTierNode = resultNode.FirstChild("AccessTier");
    if(!accessTierNode.IsNull())
    {
      m_accessTier = IntelligentTieringAccessTierMapper::GetIntelligentTieringAccessTierForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(accessTierNode.GetText()).c_str()).c_str());
      m_accessTierHasBeenSet = true;
    }
  }

  return *this;
}

void Tiering::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_daysHasBeenSet)
  {
   XmlNode daysNode = parentNode.CreateChildElement("Days");
   ss << m_days;
   daysNode.SetText(ss.str());
   ss.str("");
  }

  if(m_accessTierHasBeenSet)
  {
   XmlNode accessTierNode = parentNode.CreateChildElement("AccessTier");
   accessTierNode.SetText(IntelligentTieringAccessTierMapper::GetNameForIntelligentTieringAccessTier(m_accessTier));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
