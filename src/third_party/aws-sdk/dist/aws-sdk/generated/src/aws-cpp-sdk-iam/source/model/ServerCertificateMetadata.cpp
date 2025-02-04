/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ServerCertificateMetadata.h>
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

ServerCertificateMetadata::ServerCertificateMetadata() : 
    m_pathHasBeenSet(false),
    m_serverCertificateNameHasBeenSet(false),
    m_serverCertificateIdHasBeenSet(false),
    m_arnHasBeenSet(false),
    m_uploadDateHasBeenSet(false),
    m_expirationHasBeenSet(false)
{
}

ServerCertificateMetadata::ServerCertificateMetadata(const XmlNode& xmlNode)
  : ServerCertificateMetadata()
{
  *this = xmlNode;
}

ServerCertificateMetadata& ServerCertificateMetadata::operator =(const XmlNode& xmlNode)
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
    XmlNode serverCertificateNameNode = resultNode.FirstChild("ServerCertificateName");
    if(!serverCertificateNameNode.IsNull())
    {
      m_serverCertificateName = Aws::Utils::Xml::DecodeEscapedXmlText(serverCertificateNameNode.GetText());
      m_serverCertificateNameHasBeenSet = true;
    }
    XmlNode serverCertificateIdNode = resultNode.FirstChild("ServerCertificateId");
    if(!serverCertificateIdNode.IsNull())
    {
      m_serverCertificateId = Aws::Utils::Xml::DecodeEscapedXmlText(serverCertificateIdNode.GetText());
      m_serverCertificateIdHasBeenSet = true;
    }
    XmlNode arnNode = resultNode.FirstChild("Arn");
    if(!arnNode.IsNull())
    {
      m_arn = Aws::Utils::Xml::DecodeEscapedXmlText(arnNode.GetText());
      m_arnHasBeenSet = true;
    }
    XmlNode uploadDateNode = resultNode.FirstChild("UploadDate");
    if(!uploadDateNode.IsNull())
    {
      m_uploadDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(uploadDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_uploadDateHasBeenSet = true;
    }
    XmlNode expirationNode = resultNode.FirstChild("Expiration");
    if(!expirationNode.IsNull())
    {
      m_expiration = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(expirationNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_expirationHasBeenSet = true;
    }
  }

  return *this;
}

void ServerCertificateMetadata::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_pathHasBeenSet)
  {
      oStream << location << index << locationValue << ".Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }

  if(m_serverCertificateNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".ServerCertificateName=" << StringUtils::URLEncode(m_serverCertificateName.c_str()) << "&";
  }

  if(m_serverCertificateIdHasBeenSet)
  {
      oStream << location << index << locationValue << ".ServerCertificateId=" << StringUtils::URLEncode(m_serverCertificateId.c_str()) << "&";
  }

  if(m_arnHasBeenSet)
  {
      oStream << location << index << locationValue << ".Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }

  if(m_uploadDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".UploadDate=" << StringUtils::URLEncode(m_uploadDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

  if(m_expirationHasBeenSet)
  {
      oStream << location << index << locationValue << ".Expiration=" << StringUtils::URLEncode(m_expiration.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

}

void ServerCertificateMetadata::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_pathHasBeenSet)
  {
      oStream << location << ".Path=" << StringUtils::URLEncode(m_path.c_str()) << "&";
  }
  if(m_serverCertificateNameHasBeenSet)
  {
      oStream << location << ".ServerCertificateName=" << StringUtils::URLEncode(m_serverCertificateName.c_str()) << "&";
  }
  if(m_serverCertificateIdHasBeenSet)
  {
      oStream << location << ".ServerCertificateId=" << StringUtils::URLEncode(m_serverCertificateId.c_str()) << "&";
  }
  if(m_arnHasBeenSet)
  {
      oStream << location << ".Arn=" << StringUtils::URLEncode(m_arn.c_str()) << "&";
  }
  if(m_uploadDateHasBeenSet)
  {
      oStream << location << ".UploadDate=" << StringUtils::URLEncode(m_uploadDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
  if(m_expirationHasBeenSet)
  {
      oStream << location << ".Expiration=" << StringUtils::URLEncode(m_expiration.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
