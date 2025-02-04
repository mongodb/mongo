/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PolicyGroup.h>
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

PolicyGroup::PolicyGroup() : 
    m_groupNameHasBeenSet(false),
    m_groupIdHasBeenSet(false)
{
}

PolicyGroup::PolicyGroup(const XmlNode& xmlNode)
  : PolicyGroup()
{
  *this = xmlNode;
}

PolicyGroup& PolicyGroup::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode groupNameNode = resultNode.FirstChild("GroupName");
    if(!groupNameNode.IsNull())
    {
      m_groupName = Aws::Utils::Xml::DecodeEscapedXmlText(groupNameNode.GetText());
      m_groupNameHasBeenSet = true;
    }
    XmlNode groupIdNode = resultNode.FirstChild("GroupId");
    if(!groupIdNode.IsNull())
    {
      m_groupId = Aws::Utils::Xml::DecodeEscapedXmlText(groupIdNode.GetText());
      m_groupIdHasBeenSet = true;
    }
  }

  return *this;
}

void PolicyGroup::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_groupNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".GroupName=" << StringUtils::URLEncode(m_groupName.c_str()) << "&";
  }

  if(m_groupIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".GroupId=" << StringUtils::URLEncode(m_groupId.c_str()) << "&";
  }

}

void PolicyGroup::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_groupNameHasBeenSet)
  {
      oStream << location << ".GroupName=" << StringUtils::URLEncode(m_groupName.c_str()) << "&";
  }
  if(m_groupIdHasBeenSet)
  {
      oStream << location << ".GroupId=" << StringUtils::URLEncode(m_groupId.c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
