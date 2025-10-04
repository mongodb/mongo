/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/Role.h>
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

Role::Role() : 
    m_pathHasBeenSet(false),
    m_roleNameHasBeenSet(false),
    m_roleIdHasBeenSet(false),
    m_arnHasBeenSet(false),
    m_createDateHasBeenSet(false),
    m_assumeRolePolicyDocumentHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_maxSessionDuration(0),
    m_maxSessionDurationHasBeenSet(false),
    m_permissionsBoundaryHasBeenSet(false),
    m_tagsHasBeenSet(false),
    m_roleLastUsedHasBeenSet(false)
{
}

Role::Role(const XmlNode& xmlNode)
  : Role()
{
  *this = xmlNode;
}

Role& Role::operator =(const XmlNode& xmlNode)
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
    XmlNode roleNameNode = resultNode.FirstChild("RoleName");
    if(!roleNameNode.IsNull())
    {
      m_roleName = Aws::Utils::Xml::DecodeEscapedXmlText(roleNameNode.GetText());
      m_roleNameHasBeenSet = true;
    }
    XmlNode roleIdNode = resultNode.FirstChild("RoleId");
    if(!roleIdNode.IsNull())
    {
      m_roleId = Aws::Utils::Xml::DecodeEscapedXmlText(roleIdNode.GetText());
      m_roleIdHasBeenSet = true;
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
    XmlNode assumeRolePolicyDocumentNode = resultNode.FirstChild("AssumeRolePolicyDocument");
    if(!assumeRolePolicyDocumentNode.IsNull())
    {
      m_assumeRolePolicyDocument = Aws::Utils::Xml::DecodeEscapedXmlText(assumeRolePolicyDocumentNode.GetText());
      m_assumeRolePolicyDocumentHasBeenSet = true;
    }
    XmlNode descriptionNode = resultNode.FirstChild("Description");
    if(!descriptionNode.IsNull())
    {
      m_description = Aws::Utils::Xml::DecodeEscapedXmlText(descriptionNode.GetText());
      m_descriptionHasBeenSet = true;
    }
    XmlNode maxSessionDurationNode = resultNode.FirstChild("MaxSessionDuration");
    if(!maxSessionDurationNode.IsNull())
    {
      m_maxSessionDuration = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(maxSessionDurationNode.GetText()).c_str()).c_str());
      m_maxSessionDurationHasBeenSet = true;
    }
    XmlNode permissionsBoundaryNode = resultNode.FirstChild("PermissionsBoundary");
    if(!permissionsBoundaryNode.IsNull())
    {
      m_permissionsBoundary = permissionsBoundaryNode;
      m_permissionsBoundaryHasBeenSet = true;
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
    XmlNode roleLastUsedNode = resultNode.FirstChild("RoleLastUsed");
    if(!roleLastUsedNode.IsNull())
    {
      m_roleLastUsed = roleLastUsedNode;
      m_roleLastUsedHasBeenSet = true;
    }
  }

  return *this;
}

void Role::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_pathHasBeenSet)
  {
      oStream << location << index << locationValue << ".Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }

  if(m_roleNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  if(m_roleIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".RoleId=" << StringUtils::URLEncode(m_roleId.c_str()) << "&";
  }

  if(m_arnHasBeenSet)
  {
      oStream << location << index << locationValue << ".Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }

  if(m_createDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

  if(m_assumeRolePolicyDocumentHasBeenSet)
  {
      oStream << location << index << locationValue << ".AssumeRolePolicyDocument=" << StringUtils::URLEncode(m_assumeRolePolicyDocument.c_str()) << "&";
  }

  if(m_descriptionHasBeenSet)
  {
      oStream << location << index << locationValue << ".Description=" << StringUtils::URLEncode(m_description.c_str()) << "&";
  }

  if(m_maxSessionDurationHasBeenSet)
  {
      oStream << location << index << locationValue << ".MaxSessionDuration=" << m_maxSessionDuration << "&";
  }

  if(m_permissionsBoundaryHasBeenSet)
  {
      Aws::StringStream permissionsBoundaryLocationAndMemberSs;
      permissionsBoundaryLocationAndMemberSs << location << index << locationValue << ".PermissionsBoundary";
      m_permissionsBoundary.OutputToStream(oStream, permissionsBoundaryLocationAndMemberSs.str().c_str());
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

  if(m_roleLastUsedHasBeenSet)
  {
      Aws::StringStream roleLastUsedLocationAndMemberSs;
      roleLastUsedLocationAndMemberSs << location << index << locationValue << ".RoleLastUsed";
      m_roleLastUsed.OutputToStream(oStream, roleLastUsedLocationAndMemberSs.str().c_str());
  }

}

void Role::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_pathHasBeenSet)
  {
      oStream << location << ".Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }
  if(m_roleNameHasBeenSet)
  {
      oStream << location << ".RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }
  if(m_roleIdHasBeenSet)
  {
      oStream << location << ".RoleId=" << StringUtils::URLEncode(m_roleId.c_str()) << "&";
  }
  if(m_arnHasBeenSet)
  {
      oStream << location << ".Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }
  if(m_createDateHasBeenSet)
  {
      oStream << location << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
  if(m_assumeRolePolicyDocumentHasBeenSet)
  {
      oStream << location << ".AssumeRolePolicyDocument=" << StringUtils::URLEncode(m_assumeRolePolicyDocument.c_str()) << "&";
  }
  if(m_descriptionHasBeenSet)
  {
      oStream << location << ".Description=" << StringUtils::URLEncode(m_description.c_str()) << "&";
  }
  if(m_maxSessionDurationHasBeenSet)
  {
      oStream << location << ".MaxSessionDuration=" << m_maxSessionDuration << "&";
  }
  if(m_permissionsBoundaryHasBeenSet)
  {
      Aws::String permissionsBoundaryLocationAndMember(location);
      permissionsBoundaryLocationAndMember += ".PermissionsBoundary";
      m_permissionsBoundary.OutputToStream(oStream, permissionsBoundaryLocationAndMember.c_str());
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
  if(m_roleLastUsedHasBeenSet)
  {
      Aws::String roleLastUsedLocationAndMember(location);
      roleLastUsedLocationAndMember += ".RoleLastUsed";
      m_roleLastUsed.OutputToStream(oStream, roleLastUsedLocationAndMember.c_str());
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
