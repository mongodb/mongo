/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/GroupDetail.h>
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

GroupDetail::GroupDetail() : 
    m_pathHasBeenSet(false),
    m_groupNameHasBeenSet(false),
    m_groupIdHasBeenSet(false),
    m_arnHasBeenSet(false),
    m_createDateHasBeenSet(false),
    m_groupPolicyListHasBeenSet(false),
    m_attachedManagedPoliciesHasBeenSet(false)
{
}

GroupDetail::GroupDetail(const XmlNode& xmlNode)
  : GroupDetail()
{
  *this = xmlNode;
}

GroupDetail& GroupDetail::operator =(const XmlNode& xmlNode)
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
    XmlNode groupPolicyListNode = resultNode.FirstChild("GroupPolicyList");
    if(!groupPolicyListNode.IsNull())
    {
      XmlNode groupPolicyListMember = groupPolicyListNode.FirstChild("member");
      while(!groupPolicyListMember.IsNull())
      {
        m_groupPolicyList.push_back(groupPolicyListMember);
        groupPolicyListMember = groupPolicyListMember.NextNode("member");
      }

      m_groupPolicyListHasBeenSet = true;
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
  }

  return *this;
}

void GroupDetail::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_pathHasBeenSet)
  {
      oStream << location << index << locationValue << ".Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }

  if(m_groupNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".GroupName=" << StringUtils::URLEncode(m_groupName.c_str()) << "&";
  }

  if(m_groupIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".GroupId=" << StringUtils::URLEncode(m_groupId.c_str()) << "&";
  }

  if(m_arnHasBeenSet)
  {
      oStream << location << index << locationValue << ".Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }

  if(m_createDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

  if(m_groupPolicyListHasBeenSet)
  {
      unsigned groupPolicyListIdx = 1;
      for(auto& item : m_groupPolicyList)
      {
        Aws::StringStream groupPolicyListSs;
        groupPolicyListSs << location << index << locationValue << ".GroupPolicyList.member." << groupPolicyListIdx++;
        item.OutputToStream(oStream, groupPolicyListSs.str().c_str());
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

}

void GroupDetail::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_pathHasBeenSet)
  {
      oStream << location << ".Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }
  if(m_groupNameHasBeenSet)
  {
      oStream << location << ".GroupName=" << StringUtils::URLEncode(m_groupName.c_str()) << "&";
  }
  if(m_groupIdHasBeenSet)
  {
      oStream << location << ".GroupId=" << StringUtils::URLEncode(m_groupId.c_str()) << "&";
  }
  if(m_arnHasBeenSet)
  {
      oStream << location << ".Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }
  if(m_createDateHasBeenSet)
  {
      oStream << location << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
  if(m_groupPolicyListHasBeenSet)
  {
      unsigned groupPolicyListIdx = 1;
      for(auto& item : m_groupPolicyList)
      {
        Aws::StringStream groupPolicyListSs;
        groupPolicyListSs << location <<  ".GroupPolicyList.member." << groupPolicyListIdx++;
        item.OutputToStream(oStream, groupPolicyListSs.str().c_str());
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
}

} // namespace Model
} // namespace IAM
} // namespace Aws
