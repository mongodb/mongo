/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/RoleLastUsed.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Xml;
using namespace Aws::Utils;

namespace Aws
{
namespace IAM
{
namespace Model
{

RoleLastUsed::RoleLastUsed() : 
    m_lastUsedDateHasBeenSet(false),
    m_regionHasBeenSet(false)
{
}

RoleLastUsed::RoleLastUsed(const XmlNode& xmlNode)
  : RoleLastUsed()
{
  *this = xmlNode;
}

RoleLastUsed& RoleLastUsed::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode lastUsedDateNode = resultNode.FirstChild("LastUsedDate");
    if(!lastUsedDateNode.IsNull())
    {
      m_lastUsedDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(lastUsedDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_lastUsedDateHasBeenSet = true;
    }
    XmlNode regionNode = resultNode.FirstChild("Region");
    if(!regionNode.IsNull())
    {
      m_region = Aws::Utils::Xml::DecodeEscapedXmlText(regionNode.GetText());
      m_regionHasBeenSet = true;
    }
  }

  return *this;
}

void RoleLastUsed::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_lastUsedDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".LastUsedDate=" << StringUtils::URLEncode(m_lastUsedDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

  if(m_regionHasBeenSet)
  {
      oStream << location << index << locationValue << ".Region=" << StringUtils::URLEncode(m_region.c_str()) << "&";
  }

}

void RoleLastUsed::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_lastUsedDateHasBeenSet)
  {
      oStream << location << ".LastUsedDate=" << StringUtils::URLEncode(m_lastUsedDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
  if(m_regionHasBeenSet)
  {
      oStream << location << ".Region=" << StringUtils::URLEncode(m_region.c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
