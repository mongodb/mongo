/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/Tier.h>
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
   * <p>Container for S3 Glacier job parameters.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/GlacierJobParameters">AWS
   * API Reference</a></p>
   */
  class GlacierJobParameters
  {
  public:
    AWS_S3_API GlacierJobParameters();
    AWS_S3_API GlacierJobParameters(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API GlacierJobParameters& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>Retrieval tier at which the restore will be processed.</p>
     */
    inline const Tier& GetTier() const{ return m_tier; }
    inline bool TierHasBeenSet() const { return m_tierHasBeenSet; }
    inline void SetTier(const Tier& value) { m_tierHasBeenSet = true; m_tier = value; }
    inline void SetTier(Tier&& value) { m_tierHasBeenSet = true; m_tier = std::move(value); }
    inline GlacierJobParameters& WithTier(const Tier& value) { SetTier(value); return *this;}
    inline GlacierJobParameters& WithTier(Tier&& value) { SetTier(std::move(value)); return *this;}
    ///@}
  private:

    Tier m_tier;
    bool m_tierHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
