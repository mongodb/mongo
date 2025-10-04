/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/CloudFunctionConfiguration.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Xml;
using namespace Aws::Utils;

namespace Aws
{
namespace S3
{
namespace Model
{

CloudFunctionConfiguration::CloudFunctionConfiguration() : 
    m_idHasBeenSet(false),
    m_eventsHasBeenSet(false),
    m_cloudFunctionHasBeenSet(false),
    m_invocationRoleHasBeenSet(false)
{
}

CloudFunctionConfiguration::CloudFunctionConfiguration(const XmlNode& xmlNode)
  : CloudFunctionConfiguration()
{
  *this = xmlNode;
}

CloudFunctionConfiguration& CloudFunctionConfiguration::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode idNode = resultNode.FirstChild("Id");
    if(!idNode.IsNull())
    {
      m_id = Aws::Utils::Xml::DecodeEscapedXmlText(idNode.GetText());
      m_idHasBeenSet = true;
    }
    XmlNode eventsNode = resultNode.FirstChild("Event");
    if(!eventsNode.IsNull())
    {
      XmlNode eventMember = eventsNode;
      while(!eventMember.IsNull())
      {
        m_events.push_back(EventMapper::GetEventForName(StringUtils::Trim(eventMember.GetText().c_str())));
        eventMember = eventMember.NextNode("Event");
      }

      m_eventsHasBeenSet = true;
    }
    XmlNode cloudFunctionNode = resultNode.FirstChild("CloudFunction");
    if(!cloudFunctionNode.IsNull())
    {
      m_cloudFunction = Aws::Utils::Xml::DecodeEscapedXmlText(cloudFunctionNode.GetText());
      m_cloudFunctionHasBeenSet = true;
    }
    XmlNode invocationRoleNode = resultNode.FirstChild("InvocationRole");
    if(!invocationRoleNode.IsNull())
    {
      m_invocationRole = Aws::Utils::Xml::DecodeEscapedXmlText(invocationRoleNode.GetText());
      m_invocationRoleHasBeenSet = true;
    }
  }

  return *this;
}

void CloudFunctionConfiguration::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_idHasBeenSet)
  {
   XmlNode idNode = parentNode.CreateChildElement("Id");
   idNode.SetText(m_id);
  }

  if(m_eventsHasBeenSet)
  {
   for(const auto& item : m_events)
   {
     XmlNode eventsNode = parentNode.CreateChildElement("Event");
     eventsNode.SetText(EventMapper::GetNameForEvent(item));
   }
  }

  if(m_cloudFunctionHasBeenSet)
  {
   XmlNode cloudFunctionNode = parentNode.CreateChildElement("CloudFunction");
   cloudFunctionNode.SetText(m_cloudFunction);
  }

  if(m_invocationRoleHasBeenSet)
  {
   XmlNode invocationRoleNode = parentNode.CreateChildElement("InvocationRole");
   invocationRoleNode.SetText(m_invocationRole);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
