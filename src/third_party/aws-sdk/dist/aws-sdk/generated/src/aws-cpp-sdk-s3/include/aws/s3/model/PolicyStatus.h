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
   * <p>The container element for a bucket's policy status.</p><p><h3>See Also:</h3> 
   * <a href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/PolicyStatus">AWS
   * API Reference</a></p>
   */
  class PolicyStatus
  {
  public:
    AWS_S3_API PolicyStatus();
    AWS_S3_API PolicyStatus(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API PolicyStatus& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The policy status for this bucket. <code>TRUE</code> indicates that this
     * bucket is public. <code>FALSE</code> indicates that the bucket is not
     * public.</p>
     */
    inline bool GetIsPublic() const{ return m_isPublic; }
    inline bool IsPublicHasBeenSet() const { return m_isPublicHasBeenSet; }
    inline void SetIsPublic(bool value) { m_isPublicHasBeenSet = true; m_isPublic = value; }
    inline PolicyStatus& WithIsPublic(bool value) { SetIsPublic(value); return *this;}
    ///@}
  private:

    bool m_isPublic;
    bool m_isPublicHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
