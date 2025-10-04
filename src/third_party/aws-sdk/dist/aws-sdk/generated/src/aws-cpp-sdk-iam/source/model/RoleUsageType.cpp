/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/RoleUsageType.h>
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

RoleUsageType::RoleUsageType() : 
    m_regionHasBeenSet(false),
    m_resourcesHasBeenSet(false)
{
}

RoleUsageType::RoleUsageType(const XmlNode& xmlNode)
  : RoleUsageType()
{
  *this = xmlNode;
}

RoleUsageType& RoleUsageType::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode regionNode = resultNode.FirstChild("Region");
    if(!regionNode.IsNull())
    {
      m_region = Aws::Utils::Xml::DecodeEscapedXmlText(regionNode.GetText());
      m_regionHasBeenSet = true;
    }
    XmlNode resourcesNode = resultNode.FirstChild("Resources");
    if(!resourcesNode.IsNull())
    {
      XmlNode resourcesMember = resourcesNode.FirstChild("member");
      while(!resourcesMember.IsNull())
      {
        m_resources.push_back(resourcesMember.GetText());
        resourcesMember = resourcesMember.NextNode("member");
      }

      m_resourcesHasBeenSet = true;
    }
  }

  return *this;
}

void RoleUsageType::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_regionHasBeenSet)
  {
      oStream << location << index << locationValue << ".Region=" << StringUtils::URLEncode(m_region.c_str()) << "&";
  }

  if(m_resourcesHasBeenSet)
  {
      unsigned resourcesIdx = 1;
      for(auto& item : m_resources)
      {
        oStream << location << index << locationValue << ".Resources.member." << resourcesIdx++ << "=" << StringUtils::URLEncode(item.c_str()) << "&";
      }
  }

}

void RoleUsageType::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_regionHasBeenSet)
  {
      oStream << location << ".Region=" << StringUtils::URLEncode(m_region.c_str()) << "&";
  }
  if(m_resourcesHasBeenSet)
  {
      unsigned resourcesIdx = 1;
      for(auto& item : m_resources)
      {
        oStream << location << ".Resources.member." << resourcesIdx++ << "=" << StringUtils::URLEncode(item.c_str()) << "&";
      }
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
