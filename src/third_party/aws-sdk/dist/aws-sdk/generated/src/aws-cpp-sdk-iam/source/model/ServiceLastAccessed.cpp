/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ServiceLastAccessed.h>
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

ServiceLastAccessed::ServiceLastAccessed() : 
    m_serviceNameHasBeenSet(false),
    m_lastAuthenticatedHasBeenSet(false),
    m_serviceNamespaceHasBeenSet(false),
    m_lastAuthenticatedEntityHasBeenSet(false),
    m_lastAuthenticatedRegionHasBeenSet(false),
    m_totalAuthenticatedEntities(0),
    m_totalAuthenticatedEntitiesHasBeenSet(false),
    m_trackedActionsLastAccessedHasBeenSet(false)
{
}

ServiceLastAccessed::ServiceLastAccessed(const XmlNode& xmlNode)
  : ServiceLastAccessed()
{
  *this = xmlNode;
}

ServiceLastAccessed& ServiceLastAccessed::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode serviceNameNode = resultNode.FirstChild("ServiceName");
    if(!serviceNameNode.IsNull())
    {
      m_serviceName = Aws::Utils::Xml::DecodeEscapedXmlText(serviceNameNode.GetText());
      m_serviceNameHasBeenSet = true;
    }
    XmlNode lastAuthenticatedNode = resultNode.FirstChild("LastAuthenticated");
    if(!lastAuthenticatedNode.IsNull())
    {
      m_lastAuthenticated = DateTime(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(lastAuthenticatedNode.GetText()).c_str()).c_str(), Aws::Utils::DateFormat::ISO_8601);
      m_lastAuthenticatedHasBeenSet = true;
    }
    XmlNode serviceNamespaceNode = resultNode.FirstChild("ServiceNamespace");
    if(!serviceNamespaceNode.IsNull())
    {
      m_serviceNamespace = Aws::Utils::Xml::DecodeEscapedXmlText(serviceNamespaceNode.GetText());
      m_serviceNamespaceHasBeenSet = true;
    }
    XmlNode lastAuthenticatedEntityNode = resultNode.FirstChild("LastAuthenticatedEntity");
    if(!lastAuthenticatedEntityNode.IsNull())
    {
      m_lastAuthenticatedEntity = Aws::Utils::Xml::DecodeEscapedXmlText(lastAuthenticatedEntityNode.GetText());
      m_lastAuthenticatedEntityHasBeenSet = true;
    }
    XmlNode lastAuthenticatedRegionNode = resultNode.FirstChild("LastAuthenticatedRegion");
    if(!lastAuthenticatedRegionNode.IsNull())
    {
      m_lastAuthenticatedRegion = Aws::Utils::Xml::DecodeEscapedXmlText(lastAuthenticatedRegionNode.GetText());
      m_lastAuthenticatedRegionHasBeenSet = true;
    }
    XmlNode totalAuthenticatedEntitiesNode = resultNode.FirstChild("TotalAuthenticatedEntities");
    if(!totalAuthenticatedEntitiesNode.IsNull())
    {
      m_totalAuthenticatedEntities = StringUtils::ConvertToInt32(StringUtils::Trim(Aws::Utils::Xml::DecodeEscapedXmlText(totalAuthenticatedEntitiesNode.GetText()).c_str()).c_str());
      m_totalAuthenticatedEntitiesHasBeenSet = true;
    }
    XmlNode trackedActionsLastAccessedNode = resultNode.FirstChild("TrackedActionsLastAccessed");
    if(!trackedActionsLastAccessedNode.IsNull())
    {
      XmlNode trackedActionsLastAccessedMember = trackedActionsLastAccessedNode.FirstChild("member");
      while(!trackedActionsLastAccessedMember.IsNull())
      {
        m_trackedActionsLastAccessed.push_back(trackedActionsLastAccessedMember);
        trackedActionsLastAccessedMember = trackedActionsLastAccessedMember.NextNode("member");
      }

      m_trackedActionsLastAccessedHasBeenSet = true;
    }
  }

  return *this;
}

void ServiceLastAccessed::OutputToStream(Aws::OStream& oStream, const char* location, unsigned index, const char* locationValue) const
{
  if(m_serviceNameHasBeenSet)
  {
      oStream << location << index << locationValue << ".ServiceName=" << StringUtils::URLEncode(m_serviceName.c_str()) << "&";
  }

  if(m_lastAuthenticatedHasBeenSet)
  {
      oStream << location << index << locationValue << ".LastAuthenticated=" << StringUtils::URLEncode(m_lastAuthenticated.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }

  if(m_serviceNamespaceHasBeenSet)
  {
      oStream << location << index << locationValue << ".ServiceNamespace=" << StringUtils::URLEncode(m_serviceNamespace.c_str()) << "&";
  }

  if(m_lastAuthenticatedEntityHasBeenSet)
  {
      oStream << location << index << locationValue << ".LastAuthenticatedEntity=" << StringUtils::URLEncode(m_lastAuthenticatedEntity.c_str()) << "&";
  }

  if(m_lastAuthenticatedRegionHasBeenSet)
  {
      oStream << location << index << locationValue << ".LastAuthenticatedRegion=" << StringUtils::URLEncode(m_lastAuthenticatedRegion.c_str()) << "&";
  }

  if(m_totalAuthenticatedEntitiesHasBeenSet)
  {
      oStream << location << index << locationValue << ".TotalAuthenticatedEntities=" << m_totalAuthenticatedEntities << "&";
  }

  if(m_trackedActionsLastAccessedHasBeenSet)
  {
      unsigned trackedActionsLastAccessedIdx = 1;
      for(auto& item : m_trackedActionsLastAccessed)
      {
        Aws::StringStream trackedActionsLastAccessedSs;
        trackedActionsLastAccessedSs << location << index << locationValue << ".TrackedActionsLastAccessed.member." << trackedActionsLastAccessedIdx++;
        item.OutputToStream(oStream, trackedActionsLastAccessedSs.str().c_str());
      }
  }

}

void ServiceLastAccessed::OutputToStream(Aws::OStream& oStream, const char* location) const
{
  if(m_serviceNameHasBeenSet)
  {
      oStream << location << ".ServiceName=" << StringUtils::URLEncode(m_serviceName.c_str()) << "&";
  }
  if(m_lastAuthenticatedHasBeenSet)
  {
      oStream << location << ".LastAuthenticated=" << StringUtils::URLEncode(m_lastAuthenticated.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str()) << "&";
  }
  if(m_serviceNamespaceHasBeenSet)
  {
      oStream << location << ".ServiceNamespace=" << StringUtils::URLEncode(m_serviceNamespace.c_str()) << "&";
  }
  if(m_lastAuthenticatedEntityHasBeenSet)
  {
      oStream << location << ".LastAuthenticatedEntity=" << StringUtils::URLEncode(m_lastAuthenticatedEntity.c_str()) << "&";
  }
  if(m_lastAuthenticatedRegionHasBeenSet)
  {
      oStream << location << ".LastAuthenticatedRegion=" << StringUtils::URLEncode(m_lastAuthenticatedRegion.c_str()) << "&";
  }
  if(m_totalAuthenticatedEntitiesHasBeenSet)
  {
      oStream << location << ".TotalAuthenticatedEntities=" << m_totalAuthenticatedEntities << "&";
  }
  if(m_trackedActionsLastAccessedHasBeenSet)
  {
      unsigned trackedActionsLastAccessedIdx = 1;
      for(auto& item : m_trackedActionsLastAccessed)
      {
        Aws::StringStream trackedActionsLastAccessedSs;
        trackedActionsLastAccessedSs << location <<  ".TrackedActionsLastAccessed.member." << trackedActionsLastAccessedIdx++;
        item.OutputToStream(oStream, trackedActionsLastAccessedSs.str().c_str());
      }
  }
}

} // namespace Model
} // namespace IAM
} // namespace Aws
