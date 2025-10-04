/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/SSHPublicKey.h>
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

SSHPublicKey::SSHPublicKey() : 
    m_userNameHasBeenSet(false),
    m_sSHPublicKeyIdHasBeenSet(false),
    m_fingerprintHasBeenSet(false),
    m_sSHPublicKeyBodyHasBeenSet(false),
    m_status(StatusType::NOT_SET),
    m_statusHasBeenSet(false),
    m_uploadDateHasBeenSet(false)
{
}

SSHPublicKey::SSHPublicKey(const XmlNode& xmlNode)
  : SSHPublicKey()
{
  *this = xmlNode;
}

SSHPublicKey& SSHPublicKey::operator =(const XmlNode& xmlNode)
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
    XmlNode sSHPublicKeyIdNode = resultNode.FirstChild("SSHPublicKeyId");
    if(!sSHPublicKeyIdNode.IsNull())
    {
      m_sSHPublicKeyId = Aws::Utils::Xml::DecodeEscapedXmlText(sSHPublicKeyIdNode.GetText());
      m_sSHPublicKeyIdHasBeenSet = true;
    }
    XmlNode fingerprintNode = resultNode.FirstChild("Fingerprint");
    if(!fingerprintNode.IsNull())
    {
      m_fingerprint = Aws::Utils::Xml::DecodeEscapedXmlText(fingerprintNode.GetText());
      m_fingerprintHasBeenSet = true;
    }
    XmlNode sSHPublicKeyBodyNode = resultNode.FirstChild("SSHPublicKeyBody");
    if(!sSHPublicKeyBodyNode.IsNull())
    {
      m_sSHPublicKeyBody = Aws::Utils::Xml::DecodeEscapedXmlText(sSHPublicKeyBodyNode.GetText());
      m_sSHPublicKeyBodyHasBeenSet = true;
    }
    XmlNode statusNode = resultNode.FirstChild("Status");
    if(!statusNode.IsNull())
    {
      m_status = StatusTypeMapper::GetStatusTypeForName(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(statusNode.GetText()).c_str()).c_str());
      m_statusHasBeenSet = true;
    }
    XmlNode uploadDateNode = resultNode.FirstChild("UploadDate");
    if(!uploadDateNode.IsNull())
    {
      m_uploadDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(uploadDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_uploadDateHasBeenSet = true;
    }
  }

  return *this;
}

void SSHPublicKey::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_userNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_sSHPublicKeyIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".SSHPublicKeyId=" << StringUtils::URLEncode(m_sSHPublicKeyId.c_str()) << "&";
  }

  if(m_fingerprintHasBeenSet)
  {
      oStream << location << index << locationValue << ".Fingerprint=" << StringUtils::URLEncode(m_fingerprint.c_str()) << "&";
  }

  if(m_sSHPublicKeyBodyHasBeenSet)
  {
      oStream << location << index << locationValue << ".SSHPublicKeyBody=" << StringUtils::URLEncode(m_sSHPublicKeyBody.c_str()) << "&";
  }

  if(m_statusHasBeenSet)
  {
      oStream << location << index << locationValue << ".Status=" << StatusTypeMapper::GetNameForStatusType(m_status) << "&";
  }

  if(m_uploadDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".UploadDate=" << StringUtils::URLEncode(m_uploadDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

}

void SSHPublicKey::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_userNameHasBeenSet)
  {
      oStream << location << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }
  if(m_sSHPublicKeyIdHasBeenSet)
  {
      oStream << location << ".SSHPublicKeyId=" << StringUtils::URLEncode(m_sSHPublicKeyId.c_str()) << "&";
  }
  if(m_fingerprintHasBeenSet)
  {
      oStream << location << ".Fingerprint=" << StringUtils::URLEncode(m_fingerprint.c_str()) << "&";
  }
  if(m_sSHPublicKeyBodyHasBeenSet)
  {
      oStream << location << ".SSHPublicKeyBody=" << StringUtils::URLEncode(m_sSHPublicKeyBody.c_str()) << "&";
  }
  if(m_statusHasBeenSet)
  {
      oStream << location << ".Status=" << StatusTypeMapper::GetNameForStatusType(m_status) << "&";
  }
  if(m_uploadDateHasBeenSet)
  {
      oStream << location << ".UploadDate=" << StringUtils::URLEncode(m_uploadDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
