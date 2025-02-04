/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/Policy.h>
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

Policy::Policy() : 
    m_policyNameHasBeenSet(false),
    m_policyIdHasBeenSet(false),
    m_arnHasBeenSet(false),
    m_pathHasBeenSet(false),
    m_defaultVersionIdHasBeenSet(false),
    m_attachmentCount(0),
    m_attachmentCountHasBeenSet(false),
    m_permissionsBoundaryUsageCount(0),
    m_permissionsBoundaryUsageCountHasBeenSet(false),
    m_isAttachable(false),
    m_isAttachableHasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_createDateHasBeenSet(false),
    m_updateDateHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

Policy::Policy(const XmlNode& xmlNode)
  : Policy()
{
  *this = xmlNode;
}

Policy& Policy::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode policyNameNode = resultNode.FirstChild("PolicyName");
    if(!policyNameNode.IsNull())
    {
      m_policyName = Aws::Utils::Xml::DecodeEscapedXmlText(policyNameNode.GetText());
      m_policyNameHasBeenSet = true;
    }
    XmlNode policyIdNode = resultNode.FirstChild("PolicyId");
    if(!policyIdNode.IsNull())
    {
      m_policyId = Aws::Utils::Xml::DecodeEscapedXmlText(policyIdNode.GetText());
      m_policyIdHasBeenSet = true;
    }
    XmlNode arnNode = resultNode.FirstChild("Arn");
    if(!arnNode.IsNull())
    {
      m_arn = Aws::Utils::Xml::DecodeEscapedXmlText(arnNode.GetText());
      m_arnHasBeenSet = true;
    }
    XmlNode pathNode = resultNode.FirstChild("Path");
    if(!pathNode.IsNull())
    {
      m_path = Aws::Utils::Xml::DecodeEscapedXmlText(pathNode.GetText());
      m_pathHasBeenSet = true;
    }
    XmlNode defaultVersionIdNode = resultNode.FirstChild("DefaultVersionId");
    if(!defaultVersionIdNode.IsNull())
    {
      m_defaultVersionId = Aws::Utils::Xml::DecodeEscapedXmlText(defaultVersionIdNode.GetText());
      m_defaultVersionIdHasBeenSet = true;
    }
    XmlNode attachmentCountNode = resultNode.FirstChild("AttachmentCount");
    if(!attachmentCountNode.IsNull())
    {
      m_attachmentCount = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(attachmentCountNode.GetText()).c_str()).c_str());
      m_attachmentCountHasBeenSet = true;
    }
    XmlNode permissionsBoundaryUsageCountNode = resultNode.FirstChild("PermissionsBoundaryUsageCount");
    if(!permissionsBoundaryUsageCountNode.IsNull())
    {
      m_permissionsBoundaryUsageCount = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(permissionsBoundaryUsageCountNode.GetText()).c_str()).c_str());
      m_permissionsBoundaryUsageCountHasBeenSet = true;
    }
    XmlNode isAttachableNode = resultNode.FirstChild("IsAttachable");
    if(!isAttachableNode.IsNull())
    {
      m_isAttachable = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(isAttachableNode.GetText()).c_str()).c_str());
      m_isAttachableHasBeenSet = true;
    }
    XmlNode descriptionNode = resultNode.FirstChild("Description");
    if(!descriptionNode.IsNull())
    {
      m_description = Aws::Utils::Xml::DecodeEscapedXmlText(descriptionNode.GetText());
      m_descriptionHasBeenSet = true;
    }
    XmlNode createDateNode = resultNode.FirstChild("CreateDate");
    if(!createDateNode.IsNull())
    {
      m_createDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(createDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_createDateHasBeenSet = true;
    }
    XmlNode updateDateNode = resultNode.FirstChild("UpdateDate");
    if(!updateDateNode.IsNull())
    {
      m_updateDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(updateDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_updateDateHasBeenSet = true;
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

void Policy::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_policyNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".PolicyName=" << StringUtils::URLEncode(m_policyName.c_str()) << "&";
  }

  if(m_policyIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".PolicyId=" << StringUtils::URLEncode(m_policyId.c_str()) << "&";
  }

  if(m_arnHasBeenSet)
  {
      oStream << location << index << locationValue << ".Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }

  if(m_pathHasBeenSet)
  {
      oStream << location << index << locationValue << ".Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }

  if(m_defaultVersionIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".DefaultVersionId=" << StringUtils::URLEncode(m_defaultVersionId.c_str()) << "&";
  }

  if(m_attachmentCountHasBeenSet)
  {
      oStream << location << index << locationValue << ".AttachmentCount=" << m_attachmentCount << "&";
  }

  if(m_permissionsBoundaryUsageCountHasBeenSet)
  {
      oStream << location << index << locationValue << ".PermissionsBoundaryUsageCount=" << m_permissionsBoundaryUsageCount << "&";
  }

  if(m_isAttachableHasBeenSet)
  {
      oStream << location << index << locationValue << ".IsAttachable=" << std::boolalpha << m_isAttachable << "&";
  }

  if(m_descriptionHasBeenSet)
  {
      oStream << location << index << locationValue << ".Description=" << StringUtils::URLEncode(m_description.c_str()) << "&";
  }

  if(m_createDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

  if(m_updateDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".UpdateDate=" << StringUtils::URLEncode(m_updateDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
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

void Policy::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_policyNameHasBeenSet)
  {
      oStream << location << ".PolicyName=" << StringUtils::URLEncode(m_policyName.c_str()) << "&";
  }
  if(m_policyIdHasBeenSet)
  {
      oStream << location << ".PolicyId=" << StringUtils::URLEncode(m_policyId.c_str()) << "&";
  }
  if(m_arnHasBeenSet)
  {
      oStream << location << ".Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }
  if(m_pathHasBeenSet)
  {
      oStream << location << ".Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }
  if(m_defaultVersionIdHasBeenSet)
  {
      oStream << location << ".DefaultVersionId=" << StringUtils::URLEncode(m_defaultVersionId.c_str()) << "&";
  }
  if(m_attachmentCountHasBeenSet)
  {
      oStream << location << ".AttachmentCount=" << m_attachmentCount << "&";
  }
  if(m_permissionsBoundaryUsageCountHasBeenSet)
  {
      oStream << location << ".PermissionsBoundaryUsageCount=" << m_permissionsBoundaryUsageCount << "&";
  }
  if(m_isAttachableHasBeenSet)
  {
      oStream << location << ".IsAttachable=" << std::boolalpha << m_isAttachable << "&";
  }
  if(m_descriptionHasBeenSet)
  {
      oStream << location << ".Description=" << StringUtils::URLEncode(m_description.c_str()) << "&";
  }
  if(m_createDateHasBeenSet)
  {
      oStream << location << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
  if(m_updateDateHasBeenSet)
  {
      oStream << location << ".UpdateDate=" << StringUtils::URLEncode(m_updateDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
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
