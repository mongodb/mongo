/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/MFADevice.h>
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

MFADevice::MFADevice() : 
    m_userNameHasBeenSet(false),
    m_serialNumberHasBeenSet(false),
    m_enableDateHasBeenSet(false)
{
}

MFADevice::MFADevice(const XmlNode& xmlNode)
  : MFADevice()
{
  *this = xmlNode;
}

MFADevice& MFADevice::operator =(const XmlNode& xmlNode)
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
    XmlNode serialNumberNode = resultNode.FirstChild("SerialNumber");
    if(!serialNumberNode.IsNull())
    {
      m_serialNumber = Aws::Utils::Xml::DecodeEscapedXmlText(serialNumberNode.GetText());
      m_serialNumberHasBeenSet = true;
    }
    XmlNode enableDateNode = resultNode.FirstChild("EnableDate");
    if(!enableDateNode.IsNull())
    {
      m_enableDate = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(enableDateNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_enableDateHasBeenSet = true;
    }
  }

  return *this;
}

void MFADevice::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_userNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }

  if(m_serialNumberHasBeenSet)
  {
      oStream << location << index << locationValue << ".SerialNumber=" << StringUtils::URLEncode(m_serialNumber.c_str()) << "&";
  }

  if(m_enableDateHasBeenSet)
  {
      oStream << location << index << locationValue << ".EnableDate=" << StringUtils::URLEncode(m_enableDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

}

void MFADevice::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_userNameHasBeenSet)
  {
      oStream << location << ".UserName=" << StringUtils::URLEncode(m_userName.c_str()) << "&";
  }
  if(m_serialNumberHasBeenSet)
  {
      oStream << location << ".SerialNumber=" << StringUtils::URLEncode(m_serialNumber.c_str()) << "&";
  }
  if(m_enableDateHasBeenSet)
  {
      oStream << location << ".EnableDate=" << StringUtils::URLEncode(m_enableDate.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
