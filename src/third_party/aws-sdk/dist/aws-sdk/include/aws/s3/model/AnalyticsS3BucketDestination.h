/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/AnalyticsS3ExportFileFormat.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
   * <p>Contains information about where to publish the analytics
   * results.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/AnalyticsS3BucketDestination">AWS
   * API Reference</a></p>
   */
  class AnalyticsS3BucketDestination
  {
  public:
    AWS_S3_API AnalyticsS3BucketDestination();
    AWS_S3_API AnalyticsS3BucketDestination(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API AnalyticsS3BucketDestination& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies the file format used when exporting data to Amazon S3.</p>
     */
    inline const AnalyticsS3ExportFileFormat& GetFormat() const{ return m_format; }
    inline bool FormatHasBeenSet() const { return m_formatHasBeenSet; }
    inline void SetFormat(const AnalyticsS3ExportFileFormat& value) { m_formatHasBeenSet = true; m_format = value; }
    inline void SetFormat(AnalyticsS3ExportFileFormat&& value) { m_formatHasBeenSet = true; m_format = std::move(value); }
    inline AnalyticsS3BucketDestination& WithFormat(const AnalyticsS3ExportFileFormat& value) { SetFormat(value); return *this;}
    inline AnalyticsS3BucketDestination& WithFormat(AnalyticsS3ExportFileFormat&& value) { SetFormat(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The account ID that owns the destination S3 bucket. If no account ID is
     * provided, the owner is not validated before exporting data.</p>  <p>
     * Although this value is optional, we strongly recommend that you set it to help
     * prevent problems if the destination bucket ownership changes. </p> 
     */
    inline const Aws::String& GetBucketAccountId() const{ return m_bucketAccountId; }
    inline bool BucketAccountIdHasBeenSet() const { return m_bucketAccountIdHasBeenSet; }
    inline void SetBucketAccountId(const Aws::String& value) { m_bucketAccountIdHasBeenSet = true; m_bucketAccountId = value; }
    inline void SetBucketAccountId(Aws::String&& value) { m_bucketAccountIdHasBeenSet = true; m_bucketAccountId = std::move(value); }
    inline void SetBucketAccountId(const char* value) { m_bucketAccountIdHasBeenSet = true; m_bucketAccountId.assign(value); }
    inline AnalyticsS3BucketDestination& WithBucketAccountId(const Aws::String& value) { SetBucketAccountId(value); return *this;}
    inline AnalyticsS3BucketDestination& WithBucketAccountId(Aws::String&& value) { SetBucketAccountId(std::move(value)); return *this;}
    inline AnalyticsS3BucketDestination& WithBucketAccountId(const char* value) { SetBucketAccountId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the bucket to which data is exported.</p>
     */
    inline const Aws::String& GetBucket() const{ return m_bucket; }
    inline bool BucketHasBeenSet() const { return m_bucketHasBeenSet; }
    inline void SetBucket(const Aws::String& value) { m_bucketHasBeenSet = true; m_bucket = value; }
    inline void SetBucket(Aws::String&& value) { m_bucketHasBeenSet = true; m_bucket = std::move(value); }
    inline void SetBucket(const char* value) { m_bucketHasBeenSet = true; m_bucket.assign(value); }
    inline AnalyticsS3BucketDestination& WithBucket(const Aws::String& value) { SetBucket(value); return *this;}
    inline AnalyticsS3BucketDestination& WithBucket(Aws::String&& value) { SetBucket(std::move(value)); return *this;}
    inline AnalyticsS3BucketDestination& WithBucket(const char* value) { SetBucket(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The prefix to use when exporting data. The prefix is prepended to all
     * results.</p>
     */
    inline const Aws::String& GetPrefix() const{ return m_prefix; }
    inline bool PrefixHasBeenSet() const { return m_prefixHasBeenSet; }
    inline void SetPrefix(const Aws::String& value) { m_prefixHasBeenSet = true; m_prefix = value; }
    inline void SetPrefix(Aws::String&& value) { m_prefixHasBeenSet = true; m_prefix = std::move(value); }
    inline void SetPrefix(const char* value) { m_prefixHasBeenSet = true; m_prefix.assign(value); }
    inline AnalyticsS3BucketDestination& WithPrefix(const Aws::String& value) { SetPrefix(value); return *this;}
    inline AnalyticsS3BucketDestination& WithPrefix(Aws::String&& value) { SetPrefix(std::move(value)); return *this;}
    inline AnalyticsS3BucketDestination& WithPrefix(const char* value) { SetPrefix(value); return *this;}
    ///@}
  private:

    AnalyticsS3ExportFileFormat m_format;
    bool m_formatHasBeenSet = false;

    Aws::String m_bucketAccountId;
    bool m_bucketAccountIdHasBeenSet = false;

    Aws::String m_bucket;
    bool m_bucketHasBeenSet = false;

    Aws::String m_prefix;
    bool m_prefixHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
