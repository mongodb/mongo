/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/EventBridgeConfiguration.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/TopicConfiguration.h>
#include <aws/s3/model/QueueConfiguration.h>
#include <aws/s3/model/LambdaFunctionConfiguration.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace S3
{
namespace Model
{

  /**
   * <p>A container for specifying the notification configuration of the bucket. If
   * this element is empty, notifications are turned off for the
   * bucket.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/NotificationConfiguration">AWS
   * API Reference</a></p>
   */
  class NotificationConfiguration
  {
  public:
    AWS_S3_API NotificationConfiguration();
    AWS_S3_API NotificationConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API NotificationConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The topic to which notifications are sent and the events for which
     * notifications are generated.</p>
     */
    inline const Aws::Vector<TopicConfiguration>& GetTopicConfigurations() const{ return m_topicConfigurations; }
    inline bool TopicConfigurationsHasBeenSet() const { return m_topicConfigurationsHasBeenSet; }
    inline void SetTopicConfigurations(const Aws::Vector<TopicConfiguration>& value) { m_topicConfigurationsHasBeenSet = true; m_topicConfigurations = value; }
    inline void SetTopicConfigurations(Aws::Vector<TopicConfiguration>&& value) { m_topicConfigurationsHasBeenSet = true; m_topicConfigurations = std::move(value); }
    inline NotificationConfiguration& WithTopicConfigurations(const Aws::Vector<TopicConfiguration>& value) { SetTopicConfigurations(value); return *this;}
    inline NotificationConfiguration& WithTopicConfigurations(Aws::Vector<TopicConfiguration>&& value) { SetTopicConfigurations(std::move(value)); return *this;}
    inline NotificationConfiguration& AddTopicConfigurations(const TopicConfiguration& value) { m_topicConfigurationsHasBeenSet = true; m_topicConfigurations.push_back(value); return *this; }
    inline NotificationConfiguration& AddTopicConfigurations(TopicConfiguration&& value) { m_topicConfigurationsHasBeenSet = true; m_topicConfigurations.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The Amazon Simple Queue Service queues to publish messages to and the events
     * for which to publish messages.</p>
     */
    inline const Aws::Vector<QueueConfiguration>& GetQueueConfigurations() const{ return m_queueConfigurations; }
    inline bool QueueConfigurationsHasBeenSet() const { return m_queueConfigurationsHasBeenSet; }
    inline void SetQueueConfigurations(const Aws::Vector<QueueConfiguration>& value) { m_queueConfigurationsHasBeenSet = true; m_queueConfigurations = value; }
    inline void SetQueueConfigurations(Aws::Vector<QueueConfiguration>&& value) { m_queueConfigurationsHasBeenSet = true; m_queueConfigurations = std::move(value); }
    inline NotificationConfiguration& WithQueueConfigurations(const Aws::Vector<QueueConfiguration>& value) { SetQueueConfigurations(value); return *this;}
    inline NotificationConfiguration& WithQueueConfigurations(Aws::Vector<QueueConfiguration>&& value) { SetQueueConfigurations(std::move(value)); return *this;}
    inline NotificationConfiguration& AddQueueConfigurations(const QueueConfiguration& value) { m_queueConfigurationsHasBeenSet = true; m_queueConfigurations.push_back(value); return *this; }
    inline NotificationConfiguration& AddQueueConfigurations(QueueConfiguration&& value) { m_queueConfigurationsHasBeenSet = true; m_queueConfigurations.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Describes the Lambda functions to invoke and the events for which to invoke
     * them.</p>
     */
    inline const Aws::Vector<LambdaFunctionConfiguration>& GetLambdaFunctionConfigurations() const{ return m_lambdaFunctionConfigurations; }
    inline bool LambdaFunctionConfigurationsHasBeenSet() const { return m_lambdaFunctionConfigurationsHasBeenSet; }
    inline void SetLambdaFunctionConfigurations(const Aws::Vector<LambdaFunctionConfiguration>& value) { m_lambdaFunctionConfigurationsHasBeenSet = true; m_lambdaFunctionConfigurations = value; }
    inline void SetLambdaFunctionConfigurations(Aws::Vector<LambdaFunctionConfiguration>&& value) { m_lambdaFunctionConfigurationsHasBeenSet = true; m_lambdaFunctionConfigurations = std::move(value); }
    inline NotificationConfiguration& WithLambdaFunctionConfigurations(const Aws::Vector<LambdaFunctionConfiguration>& value) { SetLambdaFunctionConfigurations(value); return *this;}
    inline NotificationConfiguration& WithLambdaFunctionConfigurations(Aws::Vector<LambdaFunctionConfiguration>&& value) { SetLambdaFunctionConfigurations(std::move(value)); return *this;}
    inline NotificationConfiguration& AddLambdaFunctionConfigurations(const LambdaFunctionConfiguration& value) { m_lambdaFunctionConfigurationsHasBeenSet = true; m_lambdaFunctionConfigurations.push_back(value); return *this; }
    inline NotificationConfiguration& AddLambdaFunctionConfigurations(LambdaFunctionConfiguration&& value) { m_lambdaFunctionConfigurationsHasBeenSet = true; m_lambdaFunctionConfigurations.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Enables delivery of events to Amazon EventBridge.</p>
     */
    inline const EventBridgeConfiguration& GetEventBridgeConfiguration() const{ return m_eventBridgeConfiguration; }
    inline bool EventBridgeConfigurationHasBeenSet() const { return m_eventBridgeConfigurationHasBeenSet; }
    inline void SetEventBridgeConfiguration(const EventBridgeConfiguration& value) { m_eventBridgeConfigurationHasBeenSet = true; m_eventBridgeConfiguration = value; }
    inline void SetEventBridgeConfiguration(EventBridgeConfiguration&& value) { m_eventBridgeConfigurationHasBeenSet = true; m_eventBridgeConfiguration = std::move(value); }
    inline NotificationConfiguration& WithEventBridgeConfiguration(const EventBridgeConfiguration& value) { SetEventBridgeConfiguration(value); return *this;}
    inline NotificationConfiguration& WithEventBridgeConfiguration(EventBridgeConfiguration&& value) { SetEventBridgeConfiguration(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline bool RequestIdHasBeenSet() const { return m_requestIdHasBeenSet; }
    inline void SetRequestId(const Aws::String& value) { m_requestIdHasBeenSet = true; m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestIdHasBeenSet = true; m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestIdHasBeenSet = true; m_requestId.assign(value); }
    inline NotificationConfiguration& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline NotificationConfiguration& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline NotificationConfiguration& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<TopicConfiguration> m_topicConfigurations;
    bool m_topicConfigurationsHasBeenSet = false;

    Aws::Vector<QueueConfiguration> m_queueConfigurations;
    bool m_queueConfigurationsHasBeenSet = false;

    Aws::Vector<LambdaFunctionConfiguration> m_lambdaFunctionConfigurations;
    bool m_lambdaFunctionConfigurationsHasBeenSet = false;

    EventBridgeConfiguration m_eventBridgeConfiguration;
    bool m_eventBridgeConfigurationHasBeenSet = false;

    Aws::String m_requestId;
    bool m_requestIdHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
