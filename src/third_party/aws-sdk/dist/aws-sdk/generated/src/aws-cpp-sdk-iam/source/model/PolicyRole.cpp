/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PolicyRole.h>
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

PolicyRole::PolicyRole() : 
    m_roleNameHasBeenSet(false),
    m_roleIdHasBeenSet(false)
{
}

PolicyRole::PolicyRole(const XmlNode& xmlNode)
  : PolicyRole()
{
  *this = xmlNode;
}

PolicyRole& PolicyRole::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
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
  }

  return *this;
}

void PolicyRole::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_roleNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }

  if(m_roleIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".RoleId=" << StringUtils::URLEncode(m_roleId.c_str()) << "&";
  }

}

void PolicyRole::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_roleNameHasBeenSet)
  {
      oStream << location << ".RoleName=" << StringUtils::URLEncode(m_roleName.c_str()) << "&";
  }
  if(m_roleIdHasBeenSet)
  {
      oStream << location << ".RoleId=" << StringUtils::URLEncode(m_roleId.c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
