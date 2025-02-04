/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/NotificationConfigurationDeprecated.h>
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

NotificationConfigurationDeprecated::NotificationConfigurationDeprecated() : 
    m_topicConfigurationHasBeenSet(false),
    m_queueConfigurationHasBeenSet(false),
    m_cloudFunctionConfigurationHasBeenSet(false)
{
}

NotificationConfigurationDeprecated::NotificationConfigurationDeprecated(const XmlNode& xmlNode)
  : NotificationConfigurationDeprecated()
{
  *this = xmlNode;
}

NotificationConfigurationDeprecated& NotificationConfigurationDeprecated::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode topicConfigurationNode = resultNode.FirstChild("TopicConfiguration");
    if(!topicConfigurationNode.IsNull())
    {
      m_topicConfiguration = topicConfigurationNode;
      m_topicConfigurationHasBeenSet = true;
    }
    XmlNode queueConfigurationNode = resultNode.FirstChild("QueueConfiguration");
    if(!queueConfigurationNode.IsNull())
    {
      m_queueConfiguration = queueConfigurationNode;
      m_queueConfigurationHasBeenSet = true;
    }
    XmlNode cloudFunctionConfigurationNode = resultNode.FirstChild("CloudFunctionConfiguration");
    if(!cloudFunctionConfigurationNode.IsNull())
    {
      m_cloudFunctionConfiguration = cloudFunctionConfigurationNode;
      m_cloudFunctionConfigurationHasBeenSet = true;
    }
  }

  return *this;
}

void NotificationConfigurationDeprecated::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_topicConfigurationHasBeenSet)
  {
   XmlNode topicConfigurationNode = parentNode.CreateChildElement("TopicConfiguration");
   m_topicConfiguration.AddToNode(topicConfigurationNode);
  }

  if(m_queueConfigurationHasBeenSet)
  {
   XmlNode queueConfigurationNode = parentNode.CreateChildElement("QueueConfiguration");
   m_queueConfiguration.AddToNode(queueConfigurationNode);
  }

  if(m_cloudFunctionConfigurationHasBeenSet)
  {
   XmlNode cloudFunctionConfigurationNode = parentNode.CreateChildElement("CloudFunctionConfiguration");
   m_cloudFunctionConfiguration.AddToNode(cloudFunctionConfigurationNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
