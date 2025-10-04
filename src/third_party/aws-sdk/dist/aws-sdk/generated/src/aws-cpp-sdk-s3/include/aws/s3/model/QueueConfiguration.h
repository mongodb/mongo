/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/s3/model/NotificationConfigurationFilter.h>
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
   * <p>Specifies the configuration for publishing messages to an Amazon Simple Queue
   * Service (Amazon SQS) queue when Amazon S3 detects specified
   * events.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/QueueConfiguration">AWS
   * API Reference</a></p>
   */
  class QueueConfiguration
  {
  public:
    AWS_S3_API QueueConfiguration();
    AWS_S3_API QueueConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API QueueConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    
    inline const Aws::String& GetId() const{ return m_id; }
    inline bool IdHasBeenSet() const { return m_idHasBeenSet; }
    inline void SetId(const Aws::String& value) { m_idHasBeenSet = true; m_id = value; }
    inline void SetId(Aws::String&& value) { m_idHasBeenSet = true; m_id = std::move(value); }
    inline void SetId(const char* value) { m_idHasBeenSet = true; m_id.assign(value); }
    inline QueueConfiguration& WithId(const Aws::String& value) { SetId(value); return *this;}
    inline QueueConfiguration& WithId(Aws::String&& value) { SetId(std::move(value)); return *this;}
    inline QueueConfiguration& WithId(const char* value) { SetId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the Amazon SQS queue to which Amazon S3
     * publishes a message when it detects events of the specified type.</p>
     */
    inline const Aws::String& GetQueueArn() const{ return m_queueArn; }
    inline bool QueueArnHasBeenSet() const { return m_queueArnHasBeenSet; }
    inline void SetQueueArn(const Aws::String& value) { m_queueArnHasBeenSet = true; m_queueArn = value; }
    inline void SetQueueArn(Aws::String&& value) { m_queueArnHasBeenSet = true; m_queueArn = std::move(value); }
    inline void SetQueueArn(const char* value) { m_queueArnHasBeenSet = true; m_queueArn.assign(value); }
    inline QueueConfiguration& WithQueueArn(const Aws::String& value) { SetQueueArn(value); return *this;}
    inline QueueConfiguration& WithQueueArn(Aws::String&& value) { SetQueueArn(std::move(value)); return *this;}
    inline QueueConfiguration& WithQueueArn(const char* value) { SetQueueArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A collection of bucket events for which to send notifications</p>
     */
    inline const Aws::Vector<Event>& GetEvents() const{ return m_events; }
    inline bool EventsHasBeenSet() const { return m_eventsHasBeenSet; }
    inline void SetEvents(const Aws::Vector<Event>& value) { m_eventsHasBeenSet = true; m_events = value; }
    inline void SetEvents(Aws::Vector<Event>&& value) { m_eventsHasBeenSet = true; m_events = std::move(value); }
    inline QueueConfiguration& WithEvents(const Aws::Vector<Event>& value) { SetEvents(value); return *this;}
    inline QueueConfiguration& WithEvents(Aws::Vector<Event>&& value) { SetEvents(std::move(value)); return *this;}
    inline QueueConfiguration& AddEvents(const Event& value) { m_eventsHasBeenSet = true; m_events.push_back(value); return *this; }
    inline QueueConfiguration& AddEvents(Event&& value) { m_eventsHasBeenSet = true; m_events.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const NotificationConfigurationFilter& GetFilter() const{ return m_filter; }
    inline bool FilterHasBeenSet() const { return m_filterHasBeenSet; }
    inline void SetFilter(const NotificationConfigurationFilter& value) { m_filterHasBeenSet = true; m_filter = value; }
    inline void SetFilter(NotificationConfigurationFilter&& value) { m_filterHasBeenSet = true; m_filter = std::move(value); }
    inline QueueConfiguration& WithFilter(const NotificationConfigurationFilter& value) { SetFilter(value); return *this;}
    inline QueueConfiguration& WithFilter(NotificationConfigurationFilter&& value) { SetFilter(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_id;
    bool m_idHasBeenSet = false;

    Aws::String m_queueArn;
    bool m_queueArnHasBeenSet = false;

    Aws::Vector<Event> m_events;
    bool m_eventsHasBeenSet = false;

    NotificationConfigurationFilter m_filter;
    bool m_filterHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
