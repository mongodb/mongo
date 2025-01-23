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
   * <p>Specifies the days since the initiation of an incomplete multipart upload
   * that Amazon S3 will wait before permanently removing all parts of the upload.
   * For more information, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/mpuoverview.html#mpu-abort-incomplete-mpu-lifecycle-config">
   * Aborting Incomplete Multipart Uploads Using a Bucket Lifecycle Configuration</a>
   * in the <i>Amazon S3 User Guide</i>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/AbortIncompleteMultipartUpload">AWS
   * API Reference</a></p>
   */
  class AbortIncompleteMultipartUpload
  {
  public:
    AWS_S3_API AbortIncompleteMultipartUpload();
    AWS_S3_API AbortIncompleteMultipartUpload(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API AbortIncompleteMultipartUpload& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies the number of days after which Amazon S3 aborts an incomplete
     * multipart upload.</p>
     */
    inline int GetDaysAfterInitiation() const{ return m_daysAfterInitiation; }
    inline bool DaysAfterInitiationHasBeenSet() const { return m_daysAfterInitiationHasBeenSet; }
    inline void SetDaysAfterInitiation(int value) { m_daysAfterInitiationHasBeenSet = true; m_daysAfterInitiation = value; }
    inline AbortIncompleteMultipartUpload& WithDaysAfterInitiation(int value) { SetDaysAfterInitiation(value); return *this;}
    ///@}
  private:

    int m_daysAfterInitiation;
    bool m_daysAfterInitiationHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
