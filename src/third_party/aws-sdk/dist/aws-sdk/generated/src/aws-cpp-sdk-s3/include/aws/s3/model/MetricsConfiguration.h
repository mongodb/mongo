/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/MetricsFilter.h>
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
   * <p>Specifies a metrics configuration for the CloudWatch request metrics
   * (specified by the metrics configuration ID) from an Amazon S3 bucket. If you're
   * updating an existing metrics configuration, note that this is a full replacement
   * of the existing metrics configuration. If you don't include the elements you
   * want to keep, they are erased. For more information, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/API/RESTBucketPUTMetricConfiguration.html">PutBucketMetricsConfiguration</a>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/MetricsConfiguration">AWS
   * API Reference</a></p>
   */
  class MetricsConfiguration
  {
  public:
    AWS_S3_API MetricsConfiguration();
    AWS_S3_API MetricsConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API MetricsConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The ID used to identify the metrics configuration. The ID has a 64 character
     * limit and can only contain letters, numbers, periods, dashes, and
     * underscores.</p>
     */
    inline const Aws::String& GetId() const{ return m_id; }
    inline bool IdHasBeenSet() const { return m_idHasBeenSet; }
    inline void SetId(const Aws::String& value) { m_idHasBeenSet = true; m_id = value; }
    inline void SetId(Aws::String&& value) { m_idHasBeenSet = true; m_id = std::move(value); }
    inline void SetId(const char* value) { m_idHasBeenSet = true; m_id.assign(value); }
    inline MetricsConfiguration& WithId(const Aws::String& value) { SetId(value); return *this;}
    inline MetricsConfiguration& WithId(Aws::String&& value) { SetId(std::move(value)); return *this;}
    inline MetricsConfiguration& WithId(const char* value) { SetId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies a metrics configuration filter. The metrics configuration will only
     * include objects that meet the filter's criteria. A filter must be a prefix, an
     * object tag, an access point ARN, or a conjunction (MetricsAndOperator).</p>
     */
    inline const MetricsFilter& GetFilter() const{ return m_filter; }
    inline bool FilterHasBeenSet() const { return m_filterHasBeenSet; }
    inline void SetFilter(const MetricsFilter& value) { m_filterHasBeenSet = true; m_filter = value; }
    inline void SetFilter(MetricsFilter&& value) { m_filterHasBeenSet = true; m_filter = std::move(value); }
    inline MetricsConfiguration& WithFilter(const MetricsFilter& value) { SetFilter(value); return *this;}
    inline MetricsConfiguration& WithFilter(MetricsFilter&& value) { SetFilter(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_id;
    bool m_idHasBeenSet = false;

    MetricsFilter m_filter;
    bool m_filterHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
