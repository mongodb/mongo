/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/SigningCertificate.h>
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

SigningCertificate::SigningCertificate() : 
    m_userNameHasBeenSet(false),
    m_certificateIdHasBeenSet(false),
    m_certificateBodyHasBeenSet(false),
    m_status(StatusType::NOT_SET),
    m_statusHasBeenSet(false),
    m_uploadDateHasBeenSet(false)
{
}

SigningCertificate::SigningCertificate(const XmlNode& xmlNode)
  : SigningCertificate()
{
  *this = xmlNode;
}

SigningCertificate& SigningCertificate::operator =(const XmlNode& xmlNode)
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
    XmlNode certificateIdNode = resultNode.FirstChild("CertificateId");
    if(!certificateIdNode.IsNull())
    {
      m_certificateId = Aws::Utils::Xml::DecodeEscapedXmlText(certificateIdNode.GetText());
      m_certificateIdHasBeenSet = true;
    }
    XmlNode certificateBodyNode = resultNode.FirstChild("CertificateBody");
    if(!certificateBodyNode.IsNull())
    {
      m_certificateBody = Aws::Utils::Xml::DecodeEscapedXmlText(certificateBodyNode.GetText());
      m_certificateBodyHasBeenSet = true;
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

void SigningCertificate::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_userNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_certificateIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".CertificateId=" << StringUtils::URLEncode(m_certificateId.c_str()) << "&";
  }

  if(m_certificateBodyHasBeenSet)
  {
      oStream << location << index << locationValue << ".CertificateBody=" << StringUtils::URLEncode(m_certificateBody.c_str()) << "&";
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

void SigningCertificate::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_userNameHasBeenSet)
  {
      oStream << location << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }
  if(m_certificateIdHasBeenSet)
  {
      oStream << location << ".CertificateId=" << StringUtils::URLEncode(m_certificateId.c_str()) << "&";
  }
  if(m_certificateBodyHasBeenSet)
  {
      oStream << location << ".CertificateBody=" << StringUtils::URLEncode(m_certificateBody.c_str()) << "&";
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
