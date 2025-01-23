/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PolicyUser.h>
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

PolicyUser::PolicyUser() : 
    m_userNameHasBeenSet(false),
    m_userIdHasBeenSet(false)
{
}

PolicyUser::PolicyUser(const XmlNode& xmlNode)
  : PolicyUser()
{
  *this = xmlNode;
}

PolicyUser& PolicyUser::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode userNameNode = resultNode.FirstChild("UserName");
    if(!userNameNode.IsNull())
    {
      m_userName = Aws::Utils::Xml::DecodeEscapedXmlText(userNameNode.GetText());
      m_userNameHasBeenSet = true;
    }
    XmlNode userIdNode = resultNode.FirstChild("UserId");
    if(!userIdNode.IsNull())
    {
      m_userId = Aws::Utils::Xml::DecodeEscapedXmlText(userIdNode.GetText());
      m_userIdHasBeenSet = true;
    }
  }

  return *this;
}

void PolicyUser::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_userNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_userIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".UserId=" << StringUtils::URLEncode(m_userId.c_str()) << "&";
  }

}

void PolicyUser::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_userNameHasBeenSet)
  {
      oStream << location << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }
  if(m_userIdHasBeenSet)
  {
      oStream << location << ".UserId=" << StringUtils::URLEncode(m_userId.c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
