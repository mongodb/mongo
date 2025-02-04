/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Transition.h>
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

Transition::Transition() : 
    m_dateHasBeenSet(false),
    m_days(0),
    m_daysHasBeenSet(false),
    m_storageClass(TransitionStorageClass::NOT_SET),
    m_storageClassHasBeenSet(false)
{
}

Transition::Transition(const XmlNode& xmlNode)
  : Transition()
{
  *this = xmlNode;
}

Transition& Transition::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode dateNode = resultNode.FirstChild("Date");
    if(!dateNode.IsNull())
    {
      m_date = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(dateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_dateHasBeenSet = true;
    }
    XmlNode daysNode = resultNode.FirstChild("Days");
    if(!daysNode.IsNull())
    {
      m_days = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(daysNode.GetText()).c_str()).c_str());
      m_daysHasBeenSet = true;
    }
    XmlNode storageClassNode = resultNode.FirstChild("StorageClass");
    if(!storageClassNode.IsNull())
    {
      m_storageClass = TransitionStorageClassMapper::GetTransitionStorageClassForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(storageClassNode.GetText()).c_str()).c_str());
      m_storageClassHasBeenSet = true;
    }
  }

  return *this;
}

void Transition::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_dateHasBeenSet)
  {
   XmlNode dateNode = parentNode.CreateChildElement("Date");
   dateNode.SetText(m_date.ToGmtString(Aws::Utils::DateFormat::ISO_8601));
  }

  if(m_daysHasBeenSet)
  {
   XmlNode daysNode = parentNode.CreateChildElement("Days");
   ss << m_days;
   daysNode.SetText(ss.str());
   ss.str("");
  }

  if(m_storageClassHasBeenSet)
  {
   XmlNode storageClassNode = parentNode.CreateChildElement("StorageClass");
   storageClassNode.SetText(TransitionStorageClassMapper::GetNameForTransitionStorageClass(m_storageClass));
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
