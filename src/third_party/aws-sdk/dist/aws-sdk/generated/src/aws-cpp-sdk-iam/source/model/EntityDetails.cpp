/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/EntityDetails.h>
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

EntityDetails::EntityDetails() : 
    m_entityInfoHasBeenSet(false),
    m_lastAuthenticatedHasBeenSet(false)
{
}

EntityDetails::EntityDetails(const XmlNode& xmlNode)
  : EntityDetails()
{
  *this = xmlNode;
}

EntityDetails& EntityDetails::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode entityInfoNode = resultNode.FirstChild("EntityInfo");
    if(!entityInfoNode.IsNull())
    {
      m_entityInfo = entityInfoNode;
      m_entityInfoHasBeenSet = true;
    }
    XmlNode lastAuthenticatedNode = resultNode.FirstChild("LastAuthenticated");
    if(!lastAuthenticatedNode.IsNull())
    {
      m_lastAuthenticated = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(lastAuthenticatedNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_lastAuthenticatedHasBeenSet = true;
    }
  }

  return *this;
}

void EntityDetails::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_entityInfoHasBeenSet)
  {
      Aws::StringStream entityInfoLocationAndMemberSs;
      entityInfoLocationAndMemberSs << location << index << locationValue << ".EntityInfo";
      m_entityInfo.OutputToStream(oStream, entityInfoLocationAndMemberSs.str().c_str());
  }

  if(m_lastAuthenticatedHasBeenSet)
  {
      oStream << location << index << locationValue << ".LastAuthenticated=" << StringUtils::URLEncode(m_lastAuthenticated.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

}

void EntityDetails::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_entityInfoHasBeenSet)
  {
      Aws::String entityInfoLocationAndMember(location);
      entityInfoLocationAndMember += ".EntityInfo";
      m_entityInfo.OutputToStream(oStream, entityInfoLocationAndMember.c_str());
  }
  if(m_lastAuthenticatedHasBeenSet)
  {
      oStream << location << ".LastAuthenticated=" << StringUtils::URLEncode(m_lastAuthenticated.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
