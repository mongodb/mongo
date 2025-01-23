/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/LoginProfile.h>
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

LoginProfile::LoginProfile() : 
    m_userNameHasBeenSet(false),
    m_createDateHasBeenSet(false),
    m_passwordResetRequired(false),
    m_passwordResetRequiredHasBeenSet(false)
{
}

LoginProfile::LoginProfile(const XmlNode& xmlNode)
  : LoginProfile()
{
  *this = xmlNode;
}

LoginProfile& LoginProfile::operator =(const XmlNode& xmlNode)
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
    XmlNode createDateNode = resultNode.FirstChild("CreateDate");
    if(!createDateNode.IsNull())
    {
      m_createDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(createDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_createDateHasBeenSet = true;
    }
    XmlNode passwordResetRequiredNode = resultNode.FirstChild("PasswordResetRequired");
    if(!passwordResetRequiredNode.IsNull())
    {
      m_passwordResetRequired = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(passwordResetRequiredNode.GetText()).c_str()).c_str());
      m_passwordResetRequiredHasBeenSet = true;
    }
  }

  return *this;
}

void LoginProfile::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_userNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_createDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

  if(m_passwordResetRequiredHasBeenSet)
  {
      oStream << location << index << locationValue << ".PasswordResetRequired=" << std::boolalpha << m_passwordResetRequired << "&";
  }

}

void LoginProfile::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_userNameHasBeenSet)
  {
      oStream << location << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }
  if(m_createDateHasBeenSet)
  {
      oStream << location << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
  if(m_passwordResetRequiredHasBeenSet)
  {
      oStream << location << ".PasswordResetRequired=" << std::boolalpha << m_passwordResetRequired << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
