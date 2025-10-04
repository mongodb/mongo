/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ReplicationTimeValue.h>
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

ReplicationTimeValue::ReplicationTimeValue() : 
    m_minutes(0),
    m_minutesHasBeenSet(false)
{
}

ReplicationTimeValue::ReplicationTimeValue(const XmlNode& xmlNode)
  : ReplicationTimeValue()
{
  *this = xmlNode;
}

ReplicationTimeValue& ReplicationTimeValue::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode minutesNode = resultNode.FirstChild("Minutes");
    if(!minutesNode.IsNull())
    {
      m_minutes = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(minutesNode.GetText()).c_str()).c_str());
      m_minutesHasBeenSet = true;
    }
  }

  return *this;
}

void ReplicationTimeValue::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_minutesHasBeenSet)
  {
   XmlNode minutesNode = parentNode.CreateChildElement("Minutes");
   ss << m_minutes;
   minutesNode.SetText(ss.str());
   ss.str("");
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
