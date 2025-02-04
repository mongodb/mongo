/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/ReplicationTimeStatus.h>
#include <aws/s3/model/ReplicationTimeValue.h>
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
   * <p> A container specifying S3 Replication Time Control (S3 RTC) related
   * information, including whether S3 RTC is enabled and the time when all objects
   * and operations on objects must be replicated. Must be specified together with a
   * <code>Metrics</code> block. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ReplicationTime">AWS
   * API Reference</a></p>
   */
  class ReplicationTime
  {
  public:
    AWS_S3_API ReplicationTime();
    AWS_S3_API ReplicationTime(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ReplicationTime& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p> Specifies whether the replication time is enabled. </p>
     */
    inline const ReplicationTimeStatus& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const ReplicationTimeStatus& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(ReplicationTimeStatus&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline ReplicationTime& WithStatus(const ReplicationTimeStatus& value) { SetStatus(value); return *this;}
    inline ReplicationTime& WithStatus(ReplicationTimeStatus&& value) { SetStatus(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p> A container specifying the time by which replication should be complete for
     * all objects and operations on objects. </p>
     */
    inline const ReplicationTimeValue& GetTime() const{ return m_time; }
    inline bool TimeHasBeenSet() const { return m_timeHasBeenSet; }
    inline void SetTime(const ReplicationTimeValue& value) { m_timeHasBeenSet = true; m_time = value; }
    inline void SetTime(ReplicationTimeValue&& value) { m_timeHasBeenSet = true; m_time = std::move(value); }
    inline ReplicationTime& WithTime(const ReplicationTimeValue& value) { SetTime(value); return *this;}
    inline ReplicationTime& WithTime(ReplicationTimeValue&& value) { SetTime(std::move(value)); return *this;}
    ///@}
  private:

    ReplicationTimeStatus m_status;
    bool m_statusHasBeenSet = false;

    ReplicationTimeValue m_time;
    bool m_timeHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
