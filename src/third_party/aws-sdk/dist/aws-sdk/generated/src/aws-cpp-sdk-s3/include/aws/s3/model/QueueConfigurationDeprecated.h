/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/Event.h>
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
   * <p>This data type is deprecated. Use <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/API/API_QueueConfiguration.html">QueueConfiguration</a>
   * for the same purposes. This data type specifies the configuration for publishing
   * messages to an Amazon Simple Queue Service (Amazon SQS) queue when Amazon S3
   * detects specified events. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/QueueConfigurationDeprecated">AWS
   * API Reference</a></p>
   */
  class QueueConfigurationDeprecated
  {
  public:
    AWS_S3_API QueueConfigurationDeprecated();
    AWS_S3_API QueueConfigurationDeprecated(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API QueueConfigurationDeprecated& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    
    inline const Aws::String& GetId() const{ return m_id; }
    inline bool IdHasBeenSet() const { return m_idHasBeenSet; }
    inline void SetId(const Aws::String& value) { m_idHasBeenSet = true; m_id = value; }
    inline void SetId(Aws::String&& value) { m_idHasBeenSet = true; m_id = std::move(value); }
    inline void SetId(const char* value) { m_idHasBeenSet = true; m_id.assign(value); }
    inline QueueConfigurationDeprecated& WithId(const Aws::String& value) { SetId(value); return *this;}
    inline QueueConfigurationDeprecated& WithId(Aws::String&& value) { SetId(std::move(value)); return *this;}
    inline QueueConfigurationDeprecated& WithId(const char* value) { SetId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A collection of bucket events for which to send notifications.</p>
     */
    inline const Aws::Vector<Event>& GetEvents() const{ return m_events; }
    inline bool EventsHasBeenSet() const { return m_eventsHasBeenSet; }
    inline void SetEvents(const Aws::Vector<Event>& value) { m_eventsHasBeenSet = true; m_events = value; }
    inline void SetEvents(Aws::Vector<Event>&& value) { m_eventsHasBeenSet = true; m_events = std::move(value); }
    inline QueueConfigurationDeprecated& WithEvents(const Aws::Vector<Event>& value) { SetEvents(value); return *this;}
    inline QueueConfigurationDeprecated& WithEvents(Aws::Vector<Event>&& value) { SetEvents(std::move(value)); return *this;}
    inline QueueConfigurationDeprecated& AddEvents(const Event& value) { m_eventsHasBeenSet = true; m_events.push_back(value); return *this; }
    inline QueueConfigurationDeprecated& AddEvents(Event&& value) { m_eventsHasBeenSet = true; m_events.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the Amazon SQS queue to which Amazon S3
     * publishes a message when it detects events of the specified type. </p>
     */
    inline const Aws::String& GetQueue() const{ return m_queue; }
    inline bool QueueHasBeenSet() const { return m_queueHasBeenSet; }
    inline void SetQueue(const Aws::String& value) { m_queueHasBeenSet = true; m_queue = value; }
    inline void SetQueue(Aws::String&& value) { m_queueHasBeenSet = true; m_queue = std::move(value); }
    inline void SetQueue(const char* value) { m_queueHasBeenSet = true; m_queue.assign(value); }
    inline QueueConfigurationDeprecated& WithQueue(const Aws::String& value) { SetQueue(value); return *this;}
    inline QueueConfigurationDeprecated& WithQueue(Aws::String&& value) { SetQueue(std::move(value)); return *this;}
    inline QueueConfigurationDeprecated& WithQueue(const char* value) { SetQueue(value); return *this;}
    ///@}
  private:

    Aws::String m_id;
    bool m_idHasBeenSet = false;

    Aws::Vector<Event> m_events;
    bool m_eventsHasBeenSet = false;

    Aws::String m_queue;
    bool m_queueHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
