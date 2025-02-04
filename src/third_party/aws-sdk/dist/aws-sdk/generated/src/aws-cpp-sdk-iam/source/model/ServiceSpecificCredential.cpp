/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ServiceSpecificCredential.h>
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

ServiceSpecificCredential::ServiceSpecificCredential() : 
    m_createDateHasBeenSet(false),
    m_serviceNameHasBeenSet(false),
    m_serviceUserNameHasBeenSet(false),
    m_servicePasswordHasBeenSet(false),
    m_serviceSpecificCredentialIdHasBeenSet(false),
    m_userNameHasBeenSet(false),
    m_status(StatusType::NOT_SET),
    m_statusHasBeenSet(false)
{
}

ServiceSpecificCredential::ServiceSpecificCredential(const XmlNode& xmlNode)
  : ServiceSpecificCredential()
{
  *this = xmlNode;
}

ServiceSpecificCredential& ServiceSpecificCredential::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode createDateNode = resultNode.FirstChild("CreateDate");
    if(!createDateNode.IsNull())
    {
      m_createDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(createDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_createDateHasBeenSet = true;
    }
    XmlNode serviceNameNode = resultNode.FirstChild("ServiceName");
    if(!serviceNameNode.IsNull())
    {
      m_serviceName = Aws::Utils::Xml::DecodeEscapedXmlText(serviceNameNode.GetText());
      m_serviceNameHasBeenSet = true;
    }
    XmlNode serviceUserNameNode = resultNode.FirstChild("ServiceUserName");
    if(!serviceUserNameNode.IsNull())
    {
      m_serviceUserName = Aws::Utils::Xml::DecodeEscapedXmlText(serviceUserNameNode.GetText());
      m_serviceUserNameHasBeenSet = true;
    }
    XmlNode servicePasswordNode = resultNode.FirstChild("ServicePassword");
    if(!servicePasswordNode.IsNull())
    {
      m_servicePassword = Aws::Utils::Xml::DecodeEscapedXmlText(servicePasswordNode.GetText());
      m_servicePasswordHasBeenSet = true;
    }
    XmlNode serviceSpecificCredentialIdNode = resultNode.FirstChild("ServiceSpecificCredentialId");
    if(!serviceSpecificCredentialIdNode.IsNull())
    {
      m_serviceSpecificCredentialId = Aws::Utils::Xml::DecodeEscapedXmlText(serviceSpecificCredentialIdNode.GetText());
      m_serviceSpecificCredentialIdHasBeenSet = true;
    }
    XmlNode userNameNode = resultNode.FirstChild("UserName");
    if(!userNameNode.IsNull())
    {
      m_userName = Aws::Utils::Xml::DecodeEscapedXmlText(userNameNode.GetText());
      m_userNameHasBeenSet = true;
    }
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = StatusTypeMapper::GetStatusTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
      m_statusHasBeenSet = true;
    }
  }

  return *this;
}

void ServiceSpecificCredential::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_createDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

  if(m_serviceNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".ServiceName=" << StringUtils::URLEncode(m_serviceName.c_str()) << "&";
  }

  if(m_serviceUserNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".ServiceUserName=" << StringUtils::URLEncode(m_serviceUserName.c_str()) << "&";
  }

  if(m_servicePasswordHasBeenSet)
  {
      oStream << location << index << locationValue << ".ServicePassword=" << StringUtils::URLEncode(m_servicePassword.c_str()) << "&";
  }

  if(m_serviceSpecificCredentialIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".ServiceSpecificCredentialId=" << StringUtils::URLEncode(m_serviceSpecificCredentialId.c_str()) << "&";
  }

  if(m_userNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_statusHasBeenSet)
  {
      oStream << location << index << locationValue << ".Status=" << StatusTypeMapper::GetNameForStatusType(m_status) << "&";
  }

}

void ServiceSpecificCredential::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_createDateHasBeenSet)
  {
      oStream << location << ".CreateDate=" << StringUtils::URLEncode(m_createDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
  if(m_serviceNameHasBeenSet)
  {
      oStream << location << ".ServiceName=" << StringUtils::URLEncode(m_serviceName.c_str()) << "&";
  }
  if(m_serviceUserNameHasBeenSet)
  {
      oStream << location << ".ServiceUserName=" << StringUtils::URLEncode(m_serviceUserName.c_str()) << "&";
  }
  if(m_servicePasswordHasBeenSet)
  {
      oStream << location << ".ServicePassword=" << StringUtils::URLEncode(m_servicePassword.c_str()) << "&";
  }
  if(m_serviceSpecificCredentialIdHasBeenSet)
  {
      oStream << location << ".ServiceSpecificCredentialId=" << StringUtils::URLEncode(m_serviceSpecificCredentialId.c_str()) << "&";
  }
  if(m_userNameHasBeenSet)
  {
      oStream << location << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }
  if(m_statusHasBeenSet)
  {
      oStream << location << ".Status=" << StatusTypeMapper::GetNameForStatusType(m_status) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
