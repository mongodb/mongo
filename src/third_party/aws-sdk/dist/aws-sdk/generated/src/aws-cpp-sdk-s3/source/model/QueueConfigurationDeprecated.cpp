/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/QueueConfigurationDeprecated.h>
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

QueueConfigurationDeprecated::QueueConfigurationDeprecated() : 
    m_idHasBeenSet(false),
    m_eventsHasBeenSet(false),
    m_queueHasBeenSet(false)
{
}

QueueConfigurationDeprecated::QueueConfigurationDeprecated(const XmlNode& xmlNode)
  : QueueConfigurationDeprecated()
{
  *this = xmlNode;
}

QueueConfigurationDeprecated& QueueConfigurationDeprecated::operator =(const XmlNode& xmlNode)
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
    XmlNode queueNode = resultNode.FirstChild("Queue");
    if(!queueNode.IsNull())
    {
      m_queue = Aws::Utils::Xml::DecodeEscapedXmlText(queueNode.GetText());
      m_queueHasBeenSet = true;
    }
  }

  return *this;
}

void QueueConfigurationDeprecated::AddToNode(XmlNode& parentNode) const
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

  if(m_queueHasBeenSet)
  {
   XmlNode queueNode = parentNode.CreateChildElement("Queue");
   queueNode.SetText(m_queue);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
