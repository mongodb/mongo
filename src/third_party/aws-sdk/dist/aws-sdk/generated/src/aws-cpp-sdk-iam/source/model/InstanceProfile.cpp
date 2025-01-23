/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/InstanceProfile.h>
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

InstanceProfile::InstanceProfile() : 
    m_pathHasBeenSet(false),
    m_instanceProfileNameHasBeenSet(false),
    m_instanceProfileIdHasBeenSet(false),
    m_arnHasBeenSet(false),
    m_createDateHasBeenSet(false),
    m_rolesHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

InstanceProfile::InstanceProfile(const XmlNode& xmlNode)
  : InstanceProfile()
{
  *this = xmlNode;
}

InstanceProfile& InstanceProfile::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode pathNode = resultNode.FirstChild("Path");
    if(!pathNode.IsNull())
    {
      m_path = Aws::Utils::Xml::DecodeEscapedXmlText(pathNode.GetText());
      m_pathHasBeenSet = true;
    }
    XmlNode instanceProfileNameNode = resultNode.FirstChild("InstanceProfileName");
    if(!instanceProfileNameNode.IsNull())
    {
      m_instanceProfileName = Aws::Utils::Xml::DecodeEscapedXmlText(instanceProfileNameNode.GetText());
      m_instanceProfileNameHasBeenSet = true;
    }
    XmlNode instanceProfileIdNode = resultNode.FirstChild("InstanceProfileId");
    if(!instanceProfileIdNode.IsNull())
    {
      m_instanceProfileId = Aws::Utils::Xml::DecodeEscapedXmlText(instanceProfileIdNode.GetText());
      m_instanceProfileIdHasBeenSet = true;
    }
    XmlNode arnNode = resultNode.FirstChild("Arn");
    if(!arnNode.IsNull())
    {
      m_arn = Aws::Utils::Xml::DecodeEscapedXmlText(arnNode.GetText());
      m_arnHasBeenSet = true;
    }
    XmlNode createDateNode = resultNode.FirstChild("CreateDate");
    if(!createDateNode.IsNull())
    {
      m_createDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(createDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_createDateHasBeenSet = true;
    }
    XmlNode rolesNode = resultNode.FirstChild("Roles");
    if(!rolesNode.IsNull())
    {
      XmlNode rolesMember = rolesNode.FirstChild("member");
      while(!rolesMember.IsNull())
      {
        m_roles.push_back(rolesMember);
        rolesMember = rolesMember.NextNode("member");
      }

      m_rolesHasBeenSet = true;
    }
    XmlNode tagsNode = resultNode.FirstChild("Tags");
    if(!tagsNode.IsNull())
    {
      XmlNode tagsMember = tagsNode.FirstChild("member");
      while(!tagsMember.IsNull())
      {
        m_tags.push_back(tagsMember);
        tagsMember = tagsMember.NextNode("member");
      }

      m_tagsHasBeenSet = true;
    }
  }

  return *this;
}

void InstanceProfile::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_pathHasBeenSet)
  {
      oStream << location << index << locationValue << ".Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }

  if(m_instanceProfileNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".InstanceProfileName=" << StringUtils::URLEncode(m_instanceProfileName.c_str()) << "&";
  }

  if(m_instanceProfileIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".InstanceProfileId=" << StringUtils::URLEncode(m_instanceProfileId.c_str()) << "&";
  }

  if(m_arnHasBeenSet)
  {
      oStream << location << index << locationValue << ".Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }

  if(m_createDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

  if(m_rolesHasBeenSet)
  {
      unsigned rolesIdx = 1;
      for(auto& item : m_roles)
      {
        Aws::StringStream rolesSs;
        rolesSs << location << index << locationValue << ".Roles.member." << rolesIdx++;
        item.OutputToStream(oStream, rolesSs.str().c_str());
      }
  }

  if(m_tagsHasBeenSet)
  {
      unsigned tagsIdx = 1;
      for(auto& item : m_tags)
      {
        Aws::StringStream tagsSs;
        tagsSs << location << index << locationValue << ".Tags.member." << tagsIdx++;
        item.OutputToStream(oStream, tagsSs.str().c_str());
      }
  }

}

void InstanceProfile::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_pathHasBeenSet)
  {
      oStream << location << ".Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }
  if(m_instanceProfileNameHasBeenSet)
  {
      oStream << location << ".InstanceProfileName=" << StringUtils::URLEncode(m_instanceProfileName.c_str()) << "&";
  }
  if(m_instanceProfileIdHasBeenSet)
  {
      oStream << location << ".InstanceProfileId=" << StringUtils::URLEncode(m_instanceProfileId.c_str()) << "&";
  }
  if(m_arnHasBeenSet)
  {
      oStream << location << ".Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }
  if(m_createDateHasBeenSet)
  {
      oStream << location << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
  if(m_rolesHasBeenSet)
  {
      unsigned rolesIdx = 1;
      for(auto& item : m_roles)
      {
        Aws::StringStream rolesSs;
        rolesSs << location <<  ".Roles.member." << rolesIdx++;
        item.OutputToStream(oStream, rolesSs.str().c_str());
      }
  }
  if(m_tagsHasBeenSet)
  {
      unsigned tagsIdx = 1;
      for(auto& item : m_tags)
      {
        Aws::StringStream tagsSs;
        tagsSs << location <<  ".Tags.member." << tagsIdx++;
        item.OutputToStream(oStream, tagsSs.str().c_str());
      }
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
