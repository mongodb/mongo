/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>

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
   * <p> A container specifying the time value for S3 Replication Time Control (S3
   * RTC) and replication metrics <code>EventThreshold</code>. </p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ReplicationTimeValue">AWS
   * API Reference</a></p>
   */
  class ReplicationTimeValue
  {
  public:
    AWS_S3_API ReplicationTimeValue();
    AWS_S3_API ReplicationTimeValue(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ReplicationTimeValue& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p> Contains an integer specifying time in minutes. </p> <p> Valid value: 15</p>
     */
    inline int GetMinutes() const{ return m_minutes; }
    inline bool MinutesHasBeenSet() const { return m_minutesHasBeenSet; }
    inline void SetMinutes(int value) { m_minutesHasBeenSet = true; m_minutes = value; }
    inline ReplicationTimeValue& WithMinutes(int value) { SetMinutes(value); return *this;}
    ///@}
  private:

    int m_minutes;
    bool m_minutesHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
