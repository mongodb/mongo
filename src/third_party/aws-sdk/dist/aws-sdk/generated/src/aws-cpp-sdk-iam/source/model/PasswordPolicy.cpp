/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PasswordPolicy.h>
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

PasswordPolicy::PasswordPolicy() : 
    m_minimumPasswordLength(0),
    m_minimumPasswordLengthHasBeenSet(false),
    m_requireSymbols(false),
    m_requireSymbolsHasBeenSet(false),
    m_requireNumbers(false),
    m_requireNumbersHasBeenSet(false),
    m_requireUppercaseCharacters(false),
    m_requireUppercaseCharactersHasBeenSet(false),
    m_requireLowercaseCharacters(false),
    m_requireLowercaseCharactersHasBeenSet(false),
    m_allowUsersToChangePassword(false),
    m_allowUsersToChangePasswordHasBeenSet(false),
    m_expirePasswords(false),
    m_expirePasswordsHasBeenSet(false),
    m_maxPasswordAge(0),
    m_maxPasswordAgeHasBeenSet(false),
    m_passwordReusePrevention(0),
    m_passwordReusePreventionHasBeenSet(false),
    m_hardExpiry(false),
    m_hardExpiryHasBeenSet(false)
{
}

PasswordPolicy::PasswordPolicy(const XmlNode& xmlNode)
  : PasswordPolicy()
{
  *this = xmlNode;
}

PasswordPolicy& PasswordPolicy::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode minimumPasswordLengthNode = resultNode.FirstChild("MinimumPasswordLength");
    if(!minimumPasswordLengthNode.IsNull())
    {
      m_minimumPasswordLength = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(minimumPasswordLengthNode.GetText()).c_str()).c_str());
      m_minimumPasswordLengthHasBeenSet = true;
    }
    XmlNode requireSymbolsNode = resultNode.FirstChild("RequireSymbols");
    if(!requireSymbolsNode.IsNull())
    {
      m_requireSymbols = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(requireSymbolsNode.GetText()).c_str()).c_str());
      m_requireSymbolsHasBeenSet = true;
    }
    XmlNode requireNumbersNode = resultNode.FirstChild("RequireNumbers");
    if(!requireNumbersNode.IsNull())
    {
      m_requireNumbers = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(requireNumbersNode.GetText()).c_str()).c_str());
      m_requireNumbersHasBeenSet = true;
    }
    XmlNode requireUppercaseCharactersNode = resultNode.FirstChild("RequireUppercaseCharacters");
    if(!requireUppercaseCharactersNode.IsNull())
    {
      m_requireUppercaseCharacters = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(requireUppercaseCharactersNode.GetText()).c_str()).c_str());
      m_requireUppercaseCharactersHasBeenSet = true;
    }
    XmlNode requireLowercaseCharactersNode = resultNode.FirstChild("RequireLowercaseCharacters");
    if(!requireLowercaseCharactersNode.IsNull())
    {
      m_requireLowercaseCharacters = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(requireLowercaseCharactersNode.GetText()).c_str()).c_str());
      m_requireLowercaseCharactersHasBeenSet = true;
    }
    XmlNode allowUsersToChangePasswordNode = resultNode.FirstChild("AllowUsersToChangePassword");
    if(!allowUsersToChangePasswordNode.IsNull())
    {
      m_allowUsersToChangePassword = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(allowUsersToChangePasswordNode.GetText()).c_str()).c_str());
      m_allowUsersToChangePasswordHasBeenSet = true;
    }
    XmlNode expirePasswordsNode = resultNode.FirstChild("ExpirePasswords");
    if(!expirePasswordsNode.IsNull())
    {
      m_expirePasswords = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(expirePasswordsNode.GetText()).c_str()).c_str());
      m_expirePasswordsHasBeenSet = true;
    }
    XmlNode maxPasswordAgeNode = resultNode.FirstChild("MaxPasswordAge");
    if(!maxPasswordAgeNode.IsNull())
    {
      m_maxPasswordAge = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(maxPasswordAgeNode.GetText()).c_str()).c_str());
      m_maxPasswordAgeHasBeenSet = true;
    }
    XmlNode passwordReusePreventionNode = resultNode.FirstChild("PasswordReusePrevention");
    if(!passwordReusePreventionNode.IsNull())
    {
      m_passwordReusePrevention = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(passwordReusePreventionNode.GetText()).c_str()).c_str());
      m_passwordReusePreventionHasBeenSet = true;
    }
    XmlNode hardExpiryNode = resultNode.FirstChild("HardExpiry");
    if(!hardExpiryNode.IsNull())
    {
      m_hardExpiry = StringUtils::ConvertToBool(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(hardExpiryNode.GetText()).c_str()).c_str());
      m_hardExpiryHasBeenSet = true;
    }
  }

  return *this;
}

void PasswordPolicy::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_minimumPasswordLengthHasBeenSet)
  {
      oStream << location << index << locationValue << ".MinimumPasswordLength=" << m_minimumPasswordLength << "&";
  }

  if(m_requireSymbolsHasBeenSet)
  {
      oStream << location << index << locationValue << ".RequireSymbols=" << std::boolalpha << m_requireSymbols << "&";
  }

  if(m_requireNumbersHasBeenSet)
  {
      oStream << location << index << locationValue << ".RequireNumbers=" << std::boolalpha << m_requireNumbers << "&";
  }

  if(m_requireUppercaseCharactersHasBeenSet)
  {
      oStream << location << index << locationValue << ".RequireUppercaseCharacters=" << std::boolalpha << m_requireUppercaseCharacters << "&";
  }

  if(m_requireLowercaseCharactersHasBeenSet)
  {
      oStream << location << index << locationValue << ".RequireLowercaseCharacters=" << std::boolalpha << m_requireLowercaseCharacters << "&";
  }

  if(m_allowUsersToChangePasswordHasBeenSet)
  {
      oStream << location << index << locationValue << ".AllowUsersToChangePassword=" << std::boolalpha << m_allowUsersToChangePassword << "&";
  }

  if(m_expirePasswordsHasBeenSet)
  {
      oStream << location << index << locationValue << ".ExpirePasswords=" << std::boolalpha << m_expirePasswords << "&";
  }

  if(m_maxPasswordAgeHasBeenSet)
  {
      oStream << location << index << locationValue << ".MaxPasswordAge=" << m_maxPasswordAge << "&";
  }

  if(m_passwordReusePreventionHasBeenSet)
  {
      oStream << location << index << locationValue << ".PasswordReusePrevention=" << m_passwordReusePrevention << "&";
  }

  if(m_hardExpiryHasBeenSet)
  {
      oStream << location << index << locationValue << ".HardExpiry=" << std::boolalpha << m_hardExpiry << "&";
  }

}

void PasswordPolicy::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_minimumPasswordLengthHasBeenSet)
  {
      oStream << location << ".MinimumPasswordLength=" << m_minimumPasswordLength << "&";
  }
  if(m_requireSymbolsHasBeenSet)
  {
      oStream << location << ".RequireSymbols=" << std::boolalpha << m_requireSymbols << "&";
  }
  if(m_requireNumbersHasBeenSet)
  {
      oStream << location << ".RequireNumbers=" << std::boolalpha << m_requireNumbers << "&";
  }
  if(m_requireUppercaseCharactersHasBeenSet)
  {
      oStream << location << ".RequireUppercaseCharacters=" << std::boolalpha << m_requireUppercaseCharacters << "&";
  }
  if(m_requireLowercaseCharactersHasBeenSet)
  {
      oStream << location << ".RequireLowercaseCharacters=" << std::boolalpha << m_requireLowercaseCharacters << "&";
  }
  if(m_allowUsersToChangePasswordHasBeenSet)
  {
      oStream << location << ".AllowUsersToChangePassword=" << std::boolalpha << m_allowUsersToChangePassword << "&";
  }
  if(m_expirePasswordsHasBeenSet)
  {
      oStream << location << ".ExpirePasswords=" << std::boolalpha << m_expirePasswords << "&";
  }
  if(m_maxPasswordAgeHasBeenSet)
  {
      oStream << location << ".MaxPasswordAge=" << m_maxPasswordAge << "&";
  }
  if(m_passwordReusePreventionHasBeenSet)
  {
      oStream << location << ".PasswordReusePrevention=" << m_passwordReusePrevention << "&";
  }
  if(m_hardExpiryHasBeenSet)
  {
      oStream << location << ".HardExpiry=" << std::boolalpha << m_hardExpiry << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
