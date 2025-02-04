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
   * <p>A container for specifying the configuration for Lambda
   * notifications.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/LambdaFunctionConfiguration">AWS
   * API Reference</a></p>
   */
  class LambdaFunctionConfiguration
  {
  public:
    AWS_S3_API LambdaFunctionConfiguration();
    AWS_S3_API LambdaFunctionConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API LambdaFunctionConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    
    inline const Aws::String& GetId() const{ return m_id; }
    inline bool IdHasBeenSet() const { return m_idHasBeenSet; }
    inline void SetId(const Aws::String& value) { m_idHasBeenSet = true; m_id = value; }
    inline void SetId(Aws::String&& value) { m_idHasBeenSet = true; m_id = std::move(value); }
    inline void SetId(const char* value) { m_idHasBeenSet = true; m_id.assign(value); }
    inline LambdaFunctionConfiguration& WithId(const Aws::String& value) { SetId(value); return *this;}
    inline LambdaFunctionConfiguration& WithId(Aws::String&& value) { SetId(std::move(value)); return *this;}
    inline LambdaFunctionConfiguration& WithId(const char* value) { SetId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the Lambda function that Amazon S3 invokes
     * when the specified event type occurs.</p>
     */
    inline const Aws::String& GetLambdaFunctionArn() const{ return m_lambdaFunctionArn; }
    inline bool LambdaFunctionArnHasBeenSet() const { return m_lambdaFunctionArnHasBeenSet; }
    inline void SetLambdaFunctionArn(const Aws::String& value) { m_lambdaFunctionArnHasBeenSet = true; m_lambdaFunctionArn = value; }
    inline void SetLambdaFunctionArn(Aws::String&& value) { m_lambdaFunctionArnHasBeenSet = true; m_lambdaFunctionArn = std::move(value); }
    inline void SetLambdaFunctionArn(const char* value) { m_lambdaFunctionArnHasBeenSet = true; m_lambdaFunctionArn.assign(value); }
    inline LambdaFunctionConfiguration& WithLambdaFunctionArn(const Aws::String& value) { SetLambdaFunctionArn(value); return *this;}
    inline LambdaFunctionConfiguration& WithLambdaFunctionArn(Aws::String&& value) { SetLambdaFunctionArn(std::move(value)); return *this;}
    inline LambdaFunctionConfiguration& WithLambdaFunctionArn(const char* value) { SetLambdaFunctionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon S3 bucket event for which to invoke the Lambda function. For more
     * information, see <a
     * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/NotificationHowTo.html">Supported
     * Event Types</a> in the <i>Amazon S3 User Guide</i>.</p>
     */
    inline const Aws::Vector<Event>& GetEvents() const{ return m_events; }
    inline bool EventsHasBeenSet() const { return m_eventsHasBeenSet; }
    inline void SetEvents(const Aws::Vector<Event>& value) { m_eventsHasBeenSet = true; m_events = value; }
    inline void SetEvents(Aws::Vector<Event>&& value) { m_eventsHasBeenSet = true; m_events = std::move(value); }
    inline LambdaFunctionConfiguration& WithEvents(const Aws::Vector<Event>& value) { SetEvents(value); return *this;}
    inline LambdaFunctionConfiguration& WithEvents(Aws::Vector<Event>&& value) { SetEvents(std::move(value)); return *this;}
    inline LambdaFunctionConfiguration& AddEvents(const Event& value) { m_eventsHasBeenSet = true; m_events.push_back(value); return *this; }
    inline LambdaFunctionConfiguration& AddEvents(Event&& value) { m_eventsHasBeenSet = true; m_events.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const NotificationConfigurationFilter& GetFilter() const{ return m_filter; }
    inline bool FilterHasBeenSet() const { return m_filterHasBeenSet; }
    inline void SetFilter(const NotificationConfigurationFilter& value) { m_filterHasBeenSet = true; m_filter = value; }
    inline void SetFilter(NotificationConfigurationFilter&& value) { m_filterHasBeenSet = true; m_filter = std::move(value); }
    inline LambdaFunctionConfiguration& WithFilter(const NotificationConfigurationFilter& value) { SetFilter(value); return *this;}
    inline LambdaFunctionConfiguration& WithFilter(NotificationConfigurationFilter&& value) { SetFilter(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_id;
    bool m_idHasBeenSet = false;

    Aws::String m_lambdaFunctionArn;
    bool m_lambdaFunctionArnHasBeenSet = false;

    Aws::Vector<Event> m_events;
    bool m_eventsHasBeenSet = false;

    NotificationConfigurationFilter m_filter;
    bool m_filterHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
