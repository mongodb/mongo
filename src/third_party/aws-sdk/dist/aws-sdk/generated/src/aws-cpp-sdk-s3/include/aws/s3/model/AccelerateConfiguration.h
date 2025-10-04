/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/BucketAccelerateStatus.h>
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
   * <p>Configures the transfer acceleration state for an Amazon S3 bucket. For more
   * information, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/transfer-acceleration.html">Amazon
   * S3 Transfer Acceleration</a> in the <i>Amazon S3 User Guide</i>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/AccelerateConfiguration">AWS
   * API Reference</a></p>
   */
  class AccelerateConfiguration
  {
  public:
    AWS_S3_API AccelerateConfiguration();
    AWS_S3_API AccelerateConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API AccelerateConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies the transfer acceleration status of the bucket.</p>
     */
    inline const BucketAccelerateStatus& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const BucketAccelerateStatus& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(BucketAccelerateStatus&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline AccelerateConfiguration& WithStatus(const BucketAccelerateStatus& value) { SetStatus(value); return *this;}
    inline AccelerateConfiguration& WithStatus(BucketAccelerateStatus&& value) { SetStatus(std::move(value)); return *this;}
    ///@}
  private:

    BucketAccelerateStatus m_status;
    bool m_statusHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
