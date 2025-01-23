/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/MFADelete.h>
#include <aws/s3/model/BucketVersioningStatus.h>
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
   * <p>Describes the versioning state of an Amazon S3 bucket. For more information,
   * see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/API/RESTBucketPUTVersioningStatus.html">PUT
   * Bucket versioning</a> in the <i>Amazon S3 API Reference</i>.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/VersioningConfiguration">AWS
   * API Reference</a></p>
   */
  class VersioningConfiguration
  {
  public:
    AWS_S3_API VersioningConfiguration();
    AWS_S3_API VersioningConfiguration(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API VersioningConfiguration& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Specifies whether MFA delete is enabled in the bucket versioning
     * configuration. This element is only returned if the bucket has been configured
     * with MFA delete. If the bucket has never been so configured, this element is not
     * returned.</p>
     */
    inline const MFADelete& GetMFADelete() const{ return m_mFADelete; }
    inline bool MFADeleteHasBeenSet() const { return m_mFADeleteHasBeenSet; }
    inline void SetMFADelete(const MFADelete& value) { m_mFADeleteHasBeenSet = true; m_mFADelete = value; }
    inline void SetMFADelete(MFADelete&& value) { m_mFADeleteHasBeenSet = true; m_mFADelete = std::move(value); }
    inline VersioningConfiguration& WithMFADelete(const MFADelete& value) { SetMFADelete(value); return *this;}
    inline VersioningConfiguration& WithMFADelete(MFADelete&& value) { SetMFADelete(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The versioning state of the bucket.</p>
     */
    inline const BucketVersioningStatus& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const BucketVersioningStatus& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(BucketVersioningStatus&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline VersioningConfiguration& WithStatus(const BucketVersioningStatus& value) { SetStatus(value); return *this;}
    inline VersioningConfiguration& WithStatus(BucketVersioningStatus&& value) { SetStatus(std::move(value)); return *this;}
    ///@}
  private:

    MFADelete m_mFADelete;
    bool m_mFADeleteHasBeenSet = false;

    BucketVersioningStatus m_status;
    bool m_statusHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
