/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/AnalyticsS3BucketDestination.h>
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
   * <p>Where to publish the analytics results.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/AnalyticsExportDestination">AWS
   * API Reference</a></p>
   */
  class AnalyticsExportDestination
  {
  public:
    AWS_S3_API AnalyticsExportDestination();
    AWS_S3_API AnalyticsExportDestination(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API AnalyticsExportDestination& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>A destination signifying output to an S3 bucket.</p>
     */
    inline const AnalyticsS3BucketDestination& GetS3BucketDestination() const{ return m_s3BucketDestination; }
    inline bool S3BucketDestinationHasBeenSet() const { return m_s3BucketDestinationHasBeenSet; }
    inline void SetS3BucketDestination(const AnalyticsS3BucketDestination& value) { m_s3BucketDestinationHasBeenSet = true; m_s3BucketDestination = value; }
    inline void SetS3BucketDestination(AnalyticsS3BucketDestination&& value) { m_s3BucketDestinationHasBeenSet = true; m_s3BucketDestination = std::move(value); }
    inline AnalyticsExportDestination& WithS3BucketDestination(const AnalyticsS3BucketDestination& value) { SetS3BucketDestination(value); return *this;}
    inline AnalyticsExportDestination& WithS3BucketDestination(AnalyticsS3BucketDestination&& value) { SetS3BucketDestination(std::move(value)); return *this;}
    ///@}
  private:

    AnalyticsS3BucketDestination m_s3BucketDestination;
    bool m_s3BucketDestinationHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
