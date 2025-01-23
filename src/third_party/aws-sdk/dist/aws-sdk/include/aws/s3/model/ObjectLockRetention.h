/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/ObjectLockRetentionMode.h>
#include <aws/core/utils/DateTime.h>
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
   * <p>A Retention configuration for an object.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ObjectLockRetention">AWS
   * API Reference</a></p>
   */
  class ObjectLockRetention
  {
  public:
    AWS_S3_API ObjectLockRetention();
    AWS_S3_API ObjectLockRetention(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ObjectLockRetention& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Indicates the Retention mode for the specified object.</p>
     */
    inline const ObjectLockRetentionMode& GetMode() const{ return m_mode; }
    inline bool ModeHasBeenSet() const { return m_modeHasBeenSet; }
    inline void SetMode(const ObjectLockRetentionMode& value) { m_modeHasBeenSet = true; m_mode = value; }
    inline void SetMode(ObjectLockRetentionMode&& value) { m_modeHasBeenSet = true; m_mode = std::move(value); }
    inline ObjectLockRetention& WithMode(const ObjectLockRetentionMode& value) { SetMode(value); return *this;}
    inline ObjectLockRetention& WithMode(ObjectLockRetentionMode&& value) { SetMode(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date on which this Object Lock Retention will expire.</p>
     */
    inline const Aws::Utils::DateTime& GetRetainUntilDate() const{ return m_retainUntilDate; }
    inline bool RetainUntilDateHasBeenSet() const { return m_retainUntilDateHasBeenSet; }
    inline void SetRetainUntilDate(const Aws::Utils::DateTime& value) { m_retainUntilDateHasBeenSet = true; m_retainUntilDate = value; }
    inline void SetRetainUntilDate(Aws::Utils::DateTime&& value) { m_retainUntilDateHasBeenSet = true; m_retainUntilDate = std::move(value); }
    inline ObjectLockRetention& WithRetainUntilDate(const Aws::Utils::DateTime& value) { SetRetainUntilDate(value); return *this;}
    inline ObjectLockRetention& WithRetainUntilDate(Aws::Utils::DateTime&& value) { SetRetainUntilDate(std::move(value)); return *this;}
    ///@}
  private:

    ObjectLockRetentionMode m_mode;
    bool m_modeHasBeenSet = false;

    Aws::Utils::DateTime m_retainUntilDate;
    bool m_retainUntilDateHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
