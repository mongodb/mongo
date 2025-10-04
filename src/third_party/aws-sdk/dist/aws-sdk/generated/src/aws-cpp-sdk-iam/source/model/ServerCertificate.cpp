/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ServerCertificate.h>
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

ServerCertificate::ServerCertificate() : 
    m_serverCertificateMetadataHasBeenSet(false),
    m_certificateBodyHasBeenSet(false),
    m_certificateChainHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

ServerCertificate::ServerCertificate(const XmlNode& xmlNode)
  : ServerCertificate()
{
  *this = xmlNode;
}

ServerCertificate& ServerCertificate::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode serverCertificateMetadataNode = resultNode.FirstChild("ServerCertificateMetadata");
    if(!serverCertificateMetadataNode.IsNull())
    {
      m_serverCertificateMetadata = serverCertificateMetadataNode;
      m_serverCertificateMetadataHasBeenSet = true;
    }
    XmlNode certificateBodyNode = resultNode.FirstChild("CertificateBody");
    if(!certificateBodyNode.IsNull())
    {
      m_certificateBody = Aws::Utils::Xml::DecodeEscapedXmlText(certificateBodyNode.GetText());
      m_certificateBodyHasBeenSet = true;
    }
    XmlNode certificateChainNode = resultNode.FirstChild("CertificateChain");
    if(!certificateChainNode.IsNull())
    {
      m_certificateChain = Aws::Utils::Xml::DecodeEscapedXmlText(certificateChainNode.GetText());
      m_certificateChainHasBeenSet = true;
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

void ServerCertificate::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_serverCertificateMetadataHasBeenSet)
  {
      Aws::StringStream serverCertificateMetadataLocationAndMemberSs;
      serverCertificateMetadataLocationAndMemberSs << location << index << locationValue << ".ServerCertificateMetadata";
      m_serverCertificateMetadata.OutputToStream(oStream, serverCertificateMetadataLocationAndMemberSs.str().c_str());
  }

  if(m_certificateBodyHasBeenSet)
  {
      oStream << location << index << locationValue << ".CertificateBody=" << StringUtils::URLEncode(m_certificateBody.c_str()) << "&";
  }

  if(m_certificateChainHasBeenSet)
  {
      oStream << location << index << locationValue << ".CertificateChain=" << StringUtils::URLEncode(m_certificateChain.c_str()) << "&";
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

void ServerCertificate::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_serverCertificateMetadataHasBeenSet)
  {
      Aws::String serverCertificateMetadataLocationAndMember(location);
      serverCertificateMetadataLocationAndMember += ".ServerCertificateMetadata";
      m_serverCertificateMetadata.OutputToStream(oStream, serverCertificateMetadataLocationAndMember.c_str());
  }
  if(m_certificateBodyHasBeenSet)
  {
      oStream << location << ".CertificateBody=" << StringUtils::URLEncode(m_certificateBody.c_str()) << "&";
  }
  if(m_certificateChainHasBeenSet)
  {
      oStream << location << ".CertificateChain=" << StringUtils::URLEncode(m_certificateChain.c_str()) << "&";
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
