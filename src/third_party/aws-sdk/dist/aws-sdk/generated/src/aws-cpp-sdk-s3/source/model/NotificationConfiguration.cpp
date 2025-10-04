/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/NotificationConfiguration.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
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

NotificationConfiguration::NotificationConfiguration() : 
    m_topicConfigurationsHasBeenSet(false),
    m_queueConfigurationsHasBeenSet(false),
    m_lambdaFunctionConfigurationsHasBeenSet(false),
    m_eventBridgeConfigurationHasBeenSet(false),
    m_requestIdHasBeenSet(false)
{
}

NotificationConfiguration::NotificationConfiguration(const XmlNode& xmlNode)
  : NotificationConfiguration()
{
  *this = xmlNode;
}

NotificationConfiguration& NotificationConfiguration::operator =(const XmlNode& xmlNode)
{
  XmlNode resultNode = xmlNode;

  if(!resultNode.IsNull())
  {
    XmlNode topicConfigurationsNode = resultNode.FirstChild("TopicConfiguration");
    if(!topicConfigurationsNode.IsNull())
    {
      XmlNode topicConfigurationMember = topicConfigurationsNode;
      while(!topicConfigurationMember.IsNull())
      {
        m_topicConfigurations.push_back(topicConfigurationMember);
        topicConfigurationMember = topicConfigurationMember.NextNode("TopicConfiguration");
      }

      m_topicConfigurationsHasBeenSet = true;
    }
    XmlNode queueConfigurationsNode = resultNode.FirstChild("QueueConfiguration");
    if(!queueConfigurationsNode.IsNull())
    {
      XmlNode queueConfigurationMember = queueConfigurationsNode;
      while(!queueConfigurationMember.IsNull())
      {
        m_queueConfigurations.push_back(queueConfigurationMember);
        queueConfigurationMember = queueConfigurationMember.NextNode("QueueConfiguration");
      }

      m_queueConfigurationsHasBeenSet = true;
    }
    XmlNode lambdaFunctionConfigurationsNode = resultNode.FirstChild("CloudFunctionConfiguration");
    if(!lambdaFunctionConfigurationsNode.IsNull())
    {
      XmlNode cloudFunctionConfigurationMember = lambdaFunctionConfigurationsNode;
      while(!cloudFunctionConfigurationMember.IsNull())
      {
        m_lambdaFunctionConfigurations.push_back(cloudFunctionConfigurationMember);
        cloudFunctionConfigurationMember = cloudFunctionConfigurationMember.NextNode("CloudFunctionConfiguration");
      }

      m_lambdaFunctionConfigurationsHasBeenSet = true;
    }
    XmlNode eventBridgeConfigurationNode = resultNode.FirstChild("EventBridgeConfiguration");
    if(!eventBridgeConfigurationNode.IsNull())
    {
      m_eventBridgeConfiguration = eventBridgeConfigurationNode;
      m_eventBridgeConfigurationHasBeenSet = true;
    }
  }

  return *this;
}

void NotificationConfiguration::AddToNode(XmlNode& parentNode) const
{
  Aws::StringStream ss;
  if(m_topicConfigurationsHasBeenSet)
  {
   for(const auto& item : m_topicConfigurations)
   {
     XmlNode topicConfigurationsNode = parentNode.CreateChildElement("TopicConfiguration");
     item.AddToNode(topicConfigurationsNode);
   }
  }

  if(m_queueConfigurationsHasBeenSet)
  {
   for(const auto& item : m_queueConfigurations)
   {
     XmlNode queueConfigurationsNode = parentNode.CreateChildElement("QueueConfiguration");
     item.AddToNode(queueConfigurationsNode);
   }
  }

  if(m_lambdaFunctionConfigurationsHasBeenSet)
  {
   for(const auto& item : m_lambdaFunctionConfigurations)
   {
     XmlNode lambdaFunctionConfigurationsNode = parentNode.CreateChildElement("CloudFunctionConfiguration");
     item.AddToNode(lambdaFunctionConfigurationsNode);
   }
  }

  if(m_eventBridgeConfigurationHasBeenSet)
  {
   XmlNode eventBridgeConfigurationNode = parentNode.CreateChildElement("EventBridgeConfiguration");
   m_eventBridgeConfiguration.AddToNode(eventBridgeConfigurationNode);
  }

}

} // namespace Model
} // namespace S3
} // namespace Aws
