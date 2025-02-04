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
   * <p>Container for specifying the Lambda notification configuration.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/CloudFunctionConfiguration">AWS
   * API Reference</a></p>
   */
  class CloudFunctionConfiguration
  {
  public:
    AWS_S3_API CloudFunctionConfiguration();
    AWS_S3_API CloudFunctionConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API CloudFunctionConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    
    inline const Aws::String& GetId() const{ return m_id; }
    inline bool IdHasBeenSet() const { return m_idHasBeenSet; }
    inline void SetId(const Aws::String& value) { m_idHasBeenSet = true; m_id = value; }
    inline void SetId(Aws::String&& value) { m_idHasBeenSet = true; m_id = std::move(value); }
    inline void SetId(const char* value) { m_idHasBeenSet = true; m_id.assign(value); }
    inline CloudFunctionConfiguration& WithId(const Aws::String& value) { SetId(value); return *this;}
    inline CloudFunctionConfiguration& WithId(Aws::String&& value) { SetId(std::move(value)); return *this;}
    inline CloudFunctionConfiguration& WithId(const char* value) { SetId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Bucket events for which to send notifications.</p>
     */
    inline const Aws::Vector<Event>& GetEvents() const{ return m_events; }
    inline bool EventsHasBeenSet() const { return m_eventsHasBeenSet; }
    inline void SetEvents(const Aws::Vector<Event>& value) { m_eventsHasBeenSet = true; m_events = value; }
    inline void SetEvents(Aws::Vector<Event>&& value) { m_eventsHasBeenSet = true; m_events = std::move(value); }
    inline CloudFunctionConfiguration& WithEvents(const Aws::Vector<Event>& value) { SetEvents(value); return *this;}
    inline CloudFunctionConfiguration& WithEvents(Aws::Vector<Event>&& value) { SetEvents(std::move(value)); return *this;}
    inline CloudFunctionConfiguration& AddEvents(const Event& value) { m_eventsHasBeenSet = true; m_events.push_back(value); return *this; }
    inline CloudFunctionConfiguration& AddEvents(Event&& value) { m_eventsHasBeenSet = true; m_events.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>Lambda cloud function ARN that Amazon S3 can invoke when it detects events of
     * the specified type.</p>
     */
    inline const Aws::String& GetCloudFunction() const{ return m_cloudFunction; }
    inline bool CloudFunctionHasBeenSet() const { return m_cloudFunctionHasBeenSet; }
    inline void SetCloudFunction(const Aws::String& value) { m_cloudFunctionHasBeenSet = true; m_cloudFunction = value; }
    inline void SetCloudFunction(Aws::String&& value) { m_cloudFunctionHasBeenSet = true; m_cloudFunction = std::move(value); }
    inline void SetCloudFunction(const char* value) { m_cloudFunctionHasBeenSet = true; m_cloudFunction.assign(value); }
    inline CloudFunctionConfiguration& WithCloudFunction(const Aws::String& value) { SetCloudFunction(value); return *this;}
    inline CloudFunctionConfiguration& WithCloudFunction(Aws::String&& value) { SetCloudFunction(std::move(value)); return *this;}
    inline CloudFunctionConfiguration& WithCloudFunction(const char* value) { SetCloudFunction(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The role supporting the invocation of the Lambda function</p>
     */
    inline const Aws::String& GetInvocationRole() const{ return m_invocationRole; }
    inline bool InvocationRoleHasBeenSet() const { return m_invocationRoleHasBeenSet; }
    inline void SetInvocationRole(const Aws::String& value) { m_invocationRoleHasBeenSet = true; m_invocationRole = value; }
    inline void SetInvocationRole(Aws::String&& value) { m_invocationRoleHasBeenSet = true; m_invocationRole = std::move(value); }
    inline void SetInvocationRole(const char* value) { m_invocationRoleHasBeenSet = true; m_invocationRole.assign(value); }
    inline CloudFunctionConfiguration& WithInvocationRole(const Aws::String& value) { SetInvocationRole(value); return *this;}
    inline CloudFunctionConfiguration& WithInvocationRole(Aws::String&& value) { SetInvocationRole(std::move(value)); return *this;}
    inline CloudFunctionConfiguration& WithInvocationRole(const char* value) { SetInvocationRole(value); return *this;}
    ///@}
  private:

    Aws::String m_id;
    bool m_idHasBeenSet = false;

    Aws::Vector<Event> m_events;
    bool m_eventsHasBeenSet = false;

    Aws::String m_cloudFunction;
    bool m_cloudFunctionHasBeenSet = false;

    Aws::String m_invocationRole;
    bool m_invocationRoleHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
