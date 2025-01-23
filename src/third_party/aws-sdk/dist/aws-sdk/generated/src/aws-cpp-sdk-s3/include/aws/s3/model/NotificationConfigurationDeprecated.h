/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/TopicConfigurationDeprecated.h>
#include <aws/s3/model/QueueConfigurationDeprecated.h>
#include <aws/s3/model/CloudFunctionConfiguration.h>
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

  class NotificationConfigurationDeprecated
  {
  public:
    AWS_S3_API NotificationConfigurationDeprecated();
    AWS_S3_API NotificationConfigurationDeprecated(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API NotificationConfigurationDeprecated& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>This data type is deprecated. A container for specifying the configuration
     * for publication of messages to an Amazon Simple Notification Service (Amazon
     * SNS) topic when Amazon S3 detects specified events. </p>
     */
    inline const TopicConfigurationDeprecated& GetTopicConfiguration() const{ return m_topicConfiguration; }
    inline bool TopicConfigurationHasBeenSet() const { return m_topicConfigurationHasBeenSet; }
    inline void SetTopicConfiguration(const TopicConfigurationDeprecated& value) { m_topicConfigurationHasBeenSet = true; m_topicConfiguration = value; }
    inline void SetTopicConfiguration(TopicConfigurationDeprecated&& value) { m_topicConfigurationHasBeenSet = true; m_topicConfiguration = std::move(value); }
    inline NotificationConfigurationDeprecated& WithTopicConfiguration(const TopicConfigurationDeprecated& value) { SetTopicConfiguration(value); return *this;}
    inline NotificationConfigurationDeprecated& WithTopicConfiguration(TopicConfigurationDeprecated&& value) { SetTopicConfiguration(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>This data type is deprecated. This data type specifies the configuration for
     * publishing messages to an Amazon Simple Queue Service (Amazon SQS) queue when
     * Amazon S3 detects specified events. </p>
     */
    inline const QueueConfigurationDeprecated& GetQueueConfiguration() const{ return m_queueConfiguration; }
    inline bool QueueConfigurationHasBeenSet() const { return m_queueConfigurationHasBeenSet; }
    inline void SetQueueConfiguration(const QueueConfigurationDeprecated& value) { m_queueConfigurationHasBeenSet = true; m_queueConfiguration = value; }
    inline void SetQueueConfiguration(QueueConfigurationDeprecated&& value) { m_queueConfigurationHasBeenSet = true; m_queueConfiguration = std::move(value); }
    inline NotificationConfigurationDeprecated& WithQueueConfiguration(const QueueConfigurationDeprecated& value) { SetQueueConfiguration(value); return *this;}
    inline NotificationConfigurationDeprecated& WithQueueConfiguration(QueueConfigurationDeprecated&& value) { SetQueueConfiguration(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Container for specifying the Lambda notification configuration.</p>
     */
    inline const CloudFunctionConfiguration& GetCloudFunctionConfiguration() const{ return m_cloudFunctionConfiguration; }
    inline bool CloudFunctionConfigurationHasBeenSet() const { return m_cloudFunctionConfigurationHasBeenSet; }
    inline void SetCloudFunctionConfiguration(const CloudFunctionConfiguration& value) { m_cloudFunctionConfigurationHasBeenSet = true; m_cloudFunctionConfiguration = value; }
    inline void SetCloudFunctionConfiguration(CloudFunctionConfiguration&& value) { m_cloudFunctionConfigurationHasBeenSet = true; m_cloudFunctionConfiguration = std::move(value); }
    inline NotificationConfigurationDeprecated& WithCloudFunctionConfiguration(const CloudFunctionConfiguration& value) { SetCloudFunctionConfiguration(value); return *this;}
    inline NotificationConfigurationDeprecated& WithCloudFunctionConfiguration(CloudFunctionConfiguration&& value) { SetCloudFunctionConfiguration(std::move(value)); return *this;}
    ///@}
  private:

    TopicConfigurationDeprecated m_topicConfiguration;
    bool m_topicConfigurationHasBeenSet = false;

    QueueConfigurationDeprecated m_queueConfiguration;
    bool m_queueConfigurationHasBeenSet = false;

    CloudFunctionConfiguration m_cloudFunctionConfiguration;
    bool m_cloudFunctionConfigurationHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
