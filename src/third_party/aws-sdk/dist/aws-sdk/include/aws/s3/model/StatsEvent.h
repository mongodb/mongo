/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/Stats.h>
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
   * <p>Container for the Stats Event.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/StatsEvent">AWS API
   * Reference</a></p>
   */
  class StatsEvent
  {
  public:
    AWS_S3_API StatsEvent();
    AWS_S3_API StatsEvent(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API StatsEvent& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The Stats event details.</p>
     */
    inline const Stats& GetDetails() const{ return m_details; }
    inline bool DetailsHasBeenSet() const { return m_detailsHasBeenSet; }
    inline void SetDetails(const Stats& value) { m_detailsHasBeenSet = true; m_details = value; }
    inline void SetDetails(Stats&& value) { m_detailsHasBeenSet = true; m_details = std::move(value); }
    inline StatsEvent& WithDetails(const Stats& value) { SetDetails(value); return *this;}
    inline StatsEvent& WithDetails(Stats&& value) { SetDetails(std::move(value)); return *this;}
    ///@}
  private:

    Stats m_details;
    bool m_detailsHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
