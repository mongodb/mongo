/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/TrackedActionLastAccessed.h>
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

TrackedActionLastAccessed::TrackedActionLastAccessed() : 
    m_actionNameHasBeenSet(false),
    m_lastAccessedEntityHasBeenSet(false),
    m_lastAccessedTimeHasBeenSet(false),
    m_lastAccessedRegionHasBeenSet(false)
{
}

TrackedActionLastAccessed::TrackedActionLastAccessed(const XmlNode& xmlNode)
  : TrackedActionLastAccessed()
{
  *this = xmlNode;
}

TrackedActionLastAccessed& TrackedActionLastAccessed::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode actionNameNode = resultNode.FirstChild("ActionName");
    if(!actionNameNode.IsNull())
    {
      m_actionName = Aws::Utils::Xml::DecodeEscapedXmlText(actionNameNode.GetText());
      m_actionNameHasBeenSet = true;
    }
    XmlNode lastAccessedEntityNode = resultNode.FirstChild("LastAccessedEntity");
    if(!lastAccessedEntityNode.IsNull())
    {
      m_lastAccessedEntity = Aws::Utils::Xml::DecodeEscapedXmlText(lastAccessedEntityNode.GetText());
      m_lastAccessedEntityHasBeenSet = true;
    }
    XmlNode lastAccessedTimeNode = resultNode.FirstChild("LastAccessedTime");
    if(!lastAccessedTimeNode.IsNull())
    {
      m_lastAccessedTime = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(lastAccessedTimeNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_lastAccessedTimeHasBeenSet = true;
    }
    XmlNode lastAccessedRegionNode = resultNode.FirstChild("LastAccessedRegion");
    if(!lastAccessedRegionNode.IsNull())
    {
      m_lastAccessedRegion = Aws::Utils::Xml::DecodeEscapedXmlText(lastAccessedRegionNode.GetText());
      m_lastAccessedRegionHasBeenSet = true;
    }
  }

  return *this;
}

void TrackedActionLastAccessed::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_actionNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".ActionName=" << StringUtils::URLEncode(m_actionName.c_str()) << "&";
  }

  if(m_lastAccessedEntityHasBeenSet)
  {
      oStream << location << index << locationValue << ".LastAccessedEntity=" << StringUtils::URLEncode(m_lastAccessedEntity.c_str()) << "&";
  }

  if(m_lastAccessedTimeHasBeenSet)
  {
      oStream << location << index << locationValue << ".LastAccessedTime=" << StringUtils::URLEncode(m_lastAccessedTime.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

  if(m_lastAccessedRegionHasBeenSet)
  {
      oStream << location << index << locationValue << ".LastAccessedRegion=" << StringUtils::URLEncode(m_lastAccessedRegion.c_str()) << "&";
  }

}

void TrackedActionLastAccessed::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_actionNameHasBeenSet)
  {
      oStream << location << ".ActionName=" << StringUtils::URLEncode(m_actionName.c_str()) << "&";
  }
  if(m_lastAccessedEntityHasBeenSet)
  {
      oStream << location << ".LastAccessedEntity=" << StringUtils::URLEncode(m_lastAccessedEntity.c_str()) << "&";
  }
  if(m_lastAccessedTimeHasBeenSet)
  {
      oStream << location << ".LastAccessedTime=" << StringUtils::URLEncode(m_lastAccessedTime.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
  if(m_lastAccessedRegionHasBeenSet)
  {
      oStream << location << ".LastAccessedRegion=" << StringUtils::URLEncode(m_lastAccessedRegion.c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
