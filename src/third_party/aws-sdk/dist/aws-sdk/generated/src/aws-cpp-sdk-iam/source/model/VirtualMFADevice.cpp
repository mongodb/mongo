/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/VirtualMFADevice.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/HashingUtils.h>

#include <utility>

using namespace Aws::Utils::Xml;
using namespace Aws::Utils;

namespace Aws
{
namespace IAM
{
namespace Model
{

VirtualMFADevice::VirtualMFADevice() : 
    m_serialNumberHasBeenSet(false),
    m_base32StringSeedHasBeenSet(false),
    m_qRCodePNGHasBeenSet(false),
    m_userHasBeenSet(false),
    m_enableDateHasBeenSet(false),
    m_tagsHasBeenSet(false)
{
}

VirtualMFADevice::VirtualMFADevice(const XmlNode& xmlNode)
  : VirtualMFADevice()
{
  *this = xmlNode;
}

VirtualMFADevice& VirtualMFADevice::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode serialNumberNode = resultNode.FirstChild("SerialNumber");
    if(!serialNumberNode.IsNull())
    {
      m_serialNumber = Aws::Utils::Xml::DecodeEscapedXmlText(serialNumberNode.GetText());
      m_serialNumberHasBeenSet = true;
    }
    XmlNode base32StringSeedNode = resultNode.FirstChild("Base32StringSeed");
    if(!base32StringSeedNode.IsNull())
    {
      m_base32StringSeed = HashingUtils::Base64Decode(Aws::Utils::Xml::DecodeEscapedXmlText(base32StringSeedNode.GetText()));
      m_base32StringSeedHasBeenSet = true;
    }
    XmlNode qRCodePNGNode = resultNode.FirstChild("QRCodePNG");
    if(!qRCodePNGNode.IsNull())
    {
      m_qRCodePNG = HashingUtils::Base64Decode(Aws::Utils::Xml::DecodeEscapedXmlText(qRCodePNGNode.GetText()));
      m_qRCodePNGHasBeenSet = true;
    }
    XmlNode userNode = resultNode.FirstChild("User");
    if(!userNode.IsNull())
    {
      m_user = userNode;
      m_userHasBeenSet = true;
    }
    XmlNode enableDateNode = resultNode.FirstChild("EnableDate");
    if(!enableDateNode.IsNull())
    {
      m_enableDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(enableDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_enableDateHasBeenSet = true;
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

void VirtualMFADevice::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_serialNumberHasBeenSet)
  {
      oStream << location << index << locationValue << ".SerialNumber=" << StringUtils::URLEncode(m_serialNumber.c_str()) << "&";
  }

  if(m_base32StringSeedHasBeenSet)
  {
      oStream << location << index << locationValue << ".Base32StringSeed=" << StringUtils::URLEncode(HashingUtils::Base64Encode(m_base32StringSeed).c_str()) << "&";
  }

  if(m_qRCodePNGHasBeenSet)
  {
      oStream << location << index << locationValue << ".QRCodePNG=" << StringUtils::URLEncode(HashingUtils::Base64Encode(m_qRCodePNG).c_str()) << "&";
  }

  if(m_userHasBeenSet)
  {
      Aws::StringStream userLocationAndMemberSs;
      userLocationAndMemberSs << location << index << locationValue << ".User";
      m_user.OutputToStream(oStream, userLocationAndMemberSs.str().c_str());
  }

  if(m_enableDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".EnableDate=" << StringUtils::URLEncode(m_enableDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
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

void VirtualMFADevice::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_serialNumberHasBeenSet)
  {
      oStream << location << ".SerialNumber=" << StringUtils::URLEncode(m_serialNumber.c_str()) << "&";
  }
  if(m_base32StringSeedHasBeenSet)
  {
      oStream << location << ".Base32StringSeed=" << StringUtils::URLEncode(HashingUtils::Base64Encode(m_base32StringSeed).c_str()) << "&";
  }
  if(m_qRCodePNGHasBeenSet)
  {
      oStream << location << ".QRCodePNG=" << StringUtils::URLEncode(HashingUtils::Base64Encode(m_qRCodePNG).c_str()) << "&";
  }
  if(m_userHasBeenSet)
  {
      Aws::String userLocationAndMember(location);
      userLocationAndMember += ".User";
      m_user.OutputToStream(oStream, userLocationAndMember.c_str());
  }
  if(m_enableDateHasBeenSet)
  {
      oStream << location << ".EnableDate=" << StringUtils::URLEncode(m_enableDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
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
