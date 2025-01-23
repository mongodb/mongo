/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/DateTime.h>
#include <aws/s3/model/TransitionStorageClass.h>
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
   * <p>Specifies when an object transitions to a specified storage class. For more
   * information about Amazon S3 lifecycle configuration rules, see <a
   * href="https://docs.aws.amazon.com/AmazonS3/latest/dev/lifecycle-transition-general-considerations.html">Transitioning
   * Objects Using Amazon S3 Lifecycle</a> in the <i>Amazon S3 User
   * Guide</i>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/Transition">AWS API
   * Reference</a></p>
   */
  class Transition
  {
  public:
    AWS_S3_API Transition();
    AWS_S3_API Transition(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API Transition& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Indicates when objects are transitioned to the specified storage class. The
     * date value must be in ISO 8601 format. The time is always midnight UTC.</p>
     */
    inline const Aws::Utils::DateTime& GetDate() const{ return m_date; }
    inline bool DateHasBeenSet() const { return m_dateHasBeenSet; }
    inline void SetDate(const Aws::Utils::DateTime& value) { m_dateHasBeenSet = true; m_date = value; }
    inline void SetDate(Aws::Utils::DateTime&& value) { m_dateHasBeenSet = true; m_date = std::move(value); }
    inline Transition& WithDate(const Aws::Utils::DateTime& value) { SetDate(value); return *this;}
    inline Transition& WithDate(Aws::Utils::DateTime&& value) { SetDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates the number of days after creation when objects are transitioned to
     * the specified storage class. The value must be a positive integer.</p>
     */
    inline int GetDays() const{ return m_days; }
    inline bool DaysHasBeenSet() const { return m_daysHasBeenSet; }
    inline void SetDays(int value) { m_daysHasBeenSet = true; m_days = value; }
    inline Transition& WithDays(int value) { SetDays(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The storage class to which you want the object to transition.</p>
     */
    inline const TransitionStorageClass& GetStorageClass() const{ return m_storageClass; }
    inline bool StorageClassHasBeenSet() const { return m_storageClassHasBeenSet; }
    inline void SetStorageClass(const TransitionStorageClass& value) { m_storageClassHasBeenSet = true; m_storageClass = value; }
    inline void SetStorageClass(TransitionStorageClass&& value) { m_storageClassHasBeenSet = true; m_storageClass = std::move(value); }
    inline Transition& WithStorageClass(const TransitionStorageClass& value) { SetStorageClass(value); return *this;}
    inline Transition& WithStorageClass(TransitionStorageClass&& value) { SetStorageClass(std::move(value)); return *this;}
    ///@}
  private:

    Aws::Utils::DateTime m_date;
    bool m_dateHasBeenSet = false;

    int m_days;
    bool m_daysHasBeenSet = false;

    TransitionStorageClass m_storageClass;
    bool m_storageClassHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
