/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ScanRange.h>
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

ScanRange::ScanRange() : 
    m_start(0),
    m_startHasBeenSet(false),
    m_end(0),
    m_endHasBeenSet(false)
{
}

ScanRange::ScanRange(const XmlNode& xmlNode)
  : ScanRange()
{
  *this = xmlNode;
}

ScanRange& ScanRange::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode startNode = resultNode.FirstChild("Start");
    if(!startNode.IsNull())
    {
      m_start = StringUtils::ConvertToInt64(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(startNode.GetText()).c_str()).c_str());
      m_startHasBeenSet = true;
    }
    XmlNode endNode = resultNode.FirstChild("End");
    if(!endNode.IsNull())
    {
      m_end = StringUtils::ConvertToInt64(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(endNode.GetText()).c_str()).c_str());
      m_endHasBeenSet = true;
    }
  }

  return *this;
}

void ScanRange::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_startHasBeenSet)
  {
   XmlNode startNode = parentNode.CreateChildElement("Start");
   ss << m_start;
   startNode.SetText(ss.str());
   ss.str("");
  }

  if(m_endHasBeenSet)
  {
   XmlNode endNode = parentNode.CreateChildElement("End");
   ss << m_end;
   endNode.SetText(ss.str());
   ss.str("");
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
