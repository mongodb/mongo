/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/RoleDetail.h>
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

RoleDetail::RoleDetail() : 
    m_pathHasBeenSet(false),
    m_roleNameHasBeenSet(false),
    m_roleIdHasBeenSet(false),
    m_arnHasBeenSet(false),
    m_createDateHasBeenSet(false),
    m_assumeRolePolicyDocumentHasBeenSet(false),
    m_instanceProfileListHasBeenSet(false),
    m_rolePolicyListHasBeenSet(false),
    m_attachedManagedPoliciesHasBeenSet(false),
    m_permissionsBoundaryHasBeenSet(false),
    m_tagsHasBeenSet(false),
    m_roleLastUsedHasBeenSet(false)
{
}

RoleDetail::RoleDetail(const XmlNode& xmlNode)
  : RoleDetail()
{
  *this = xmlNode;
}

RoleDetail& RoleDetail::operator =(const XmlNode& xmlNode)
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
    XmlNode instanceProfileListNode = resultNode.FirstChild("InstanceProfileList");
    if(!instanceProfileListNode.IsNull())
    {
      XmlNode instanceProfileListMember = instanceProfileListNode.FirstChild("member");
      while(!instanceProfileListMember.IsNull())
      {
        m_instanceProfileList.push_back(instanceProfileListMember);
        instanceProfileListMember = instanceProfileListMember.NextNode("member");
      }

      m_instanceProfileListHasBeenSet = true;
    }
    XmlNode rolePolicyListNode = resultNode.FirstChild("RolePolicyList");
    if(!rolePolicyListNode.IsNull())
    {
      XmlNode rolePolicyListMember = rolePolicyListNode.FirstChild("member");
      while(!rolePolicyListMember.IsNull())
      {
        m_rolePolicyList.push_back(rolePolicyListMember);
        rolePolicyListMember = rolePolicyListMember.NextNode("member");
      }

      m_rolePolicyListHasBeenSet = true;
    }
    XmlNode attachedManagedPoliciesNode = resultNode.FirstChild("AttachedManagedPolicies");
    if(!attachedManagedPoliciesNode.IsNull())
    {
      XmlNode attachedManagedPoliciesMember = attachedManagedPoliciesNode.FirstChild("member");
      while(!attachedManagedPoliciesMember.IsNull())
      {
        m_attachedManagedPolicies.push_back(attachedManagedPoliciesMember);
        attachedManagedPoliciesMember = attachedManagedPoliciesMember.NextNode("member");
      }

      m_attachedManagedPoliciesHasBeenSet = true;
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

void RoleDetail::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
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

  if(m_instanceProfileListHasBeenSet)
  {
      unsigned instanceProfileListIdx = 1;
      for(auto& item : m_instanceProfileList)
      {
        Aws::StringStream instanceProfileListSs;
        instanceProfileListSs << location << index << locationValue << ".InstanceProfileList.member." << instanceProfileListIdx++;
        item.OutputToStream(oStream, instanceProfileListSs.str().c_str());
      }
  }

  if(m_rolePolicyListHasBeenSet)
  {
      unsigned rolePolicyListIdx = 1;
      for(auto& item : m_rolePolicyList)
      {
        Aws::StringStream rolePolicyListSs;
        rolePolicyListSs << location << index << locationValue << ".RolePolicyList.member." << rolePolicyListIdx++;
        item.OutputToStream(oStream, rolePolicyListSs.str().c_str());
      }
  }

  if(m_attachedManagedPoliciesHasBeenSet)
  {
      unsigned attachedManagedPoliciesIdx = 1;
      for(auto& item : m_attachedManagedPolicies)
      {
        Aws::StringStream attachedManagedPoliciesSs;
        attachedManagedPoliciesSs << location << index << locationValue << ".AttachedManagedPolicies.member." << attachedManagedPoliciesIdx++;
        item.OutputToStream(oStream, attachedManagedPoliciesSs.str().c_str());
      }
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

void RoleDetail::OutputToStream(Aws::OStream& oStream, const char* location) const
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
  if(m_instanceProfileListHasBeenSet)
  {
      unsigned instanceProfileListIdx = 1;
      for(auto& item : m_instanceProfileList)
      {
        Aws::StringStream instanceProfileListSs;
        instanceProfileListSs << location <<  ".InstanceProfileList.member." << instanceProfileListIdx++;
        item.OutputToStream(oStream, instanceProfileListSs.str().c_str());
      }
  }
  if(m_rolePolicyListHasBeenSet)
  {
      unsigned rolePolicyListIdx = 1;
      for(auto& item : m_rolePolicyList)
      {
        Aws::StringStream rolePolicyListSs;
        rolePolicyListSs << location <<  ".RolePolicyList.member." << rolePolicyListIdx++;
        item.OutputToStream(oStream, rolePolicyListSs.str().c_str());
      }
  }
  if(m_attachedManagedPoliciesHasBeenSet)
  {
      unsigned attachedManagedPoliciesIdx = 1;
      for(auto& item : m_attachedManagedPolicies)
      {
        Aws::StringStream attachedManagedPoliciesSs;
        attachedManagedPoliciesSs << location <<  ".AttachedManagedPolicies.member." << attachedManagedPoliciesIdx++;
        item.OutputToStream(oStream, attachedManagedPoliciesSs.str().c_str());
      }
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
