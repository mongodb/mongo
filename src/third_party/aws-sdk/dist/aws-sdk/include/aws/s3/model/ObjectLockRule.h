/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/DefaultRetention.h>
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
   * <p>The container element for an Object Lock rule.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/ObjectLockRule">AWS
   * API Reference</a></p>
   */
  class ObjectLockRule
  {
  public:
    AWS_S3_API ObjectLockRule();
    AWS_S3_API ObjectLockRule(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API ObjectLockRule& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The default Object Lock retention mode and period that you want to apply to
     * new objects placed in the specified bucket. Bucket settings require both a mode
     * and a period. The period can be either <code>Days</code> or <code>Years</code>
     * but you must select one. You cannot specify <code>Days</code> and
     * <code>Years</code> at the same time.</p>
     */
    inline const DefaultRetention& GetDefaultRetention() const{ return m_defaultRetention; }
    inline bool DefaultRetentionHasBeenSet() const { return m_defaultRetentionHasBeenSet; }
    inline void SetDefaultRetention(const DefaultRetention& value) { m_defaultRetentionHasBeenSet = true; m_defaultRetention = value; }
    inline void SetDefaultRetention(DefaultRetention&& value) { m_defaultRetentionHasBeenSet = true; m_defaultRetention = std::move(value); }
    inline ObjectLockRule& WithDefaultRetention(const DefaultRetention& value) { SetDefaultRetention(value); return *this;}
    inline ObjectLockRule& WithDefaultRetention(DefaultRetention&& value) { SetDefaultRetention(std::move(value)); return *this;}
    ///@}
  private:

    DefaultRetention m_defaultRetention;
    bool m_defaultRetentionHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
