/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/ObjectOwnership.h>
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
   * <p>The container element for an ownership control rule.</p><p><h3>See Also:</h3>
   * <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/OwnershipControlsRule">AWS
   * API Reference</a></p>
   */
  class OwnershipControlsRule
  {
  public:
    AWS_S3_API OwnershipControlsRule();
    AWS_S3_API OwnershipControlsRule(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API OwnershipControlsRule& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    
    inline const ObjectOwnership& GetObjectOwnership() const{ return m_objectOwnership; }
    inline bool ObjectOwnershipHasBeenSet() const { return m_objectOwnershipHasBeenSet; }
    inline void SetObjectOwnership(const ObjectOwnership& value) { m_objectOwnershipHasBeenSet = true; m_objectOwnership = value; }
    inline void SetObjectOwnership(ObjectOwnership&& value) { m_objectOwnershipHasBeenSet = true; m_objectOwnership = std::move(value); }
    inline OwnershipControlsRule& WithObjectOwnership(const ObjectOwnership& value) { SetObjectOwnership(value); return *this;}
    inline OwnershipControlsRule& WithObjectOwnership(ObjectOwnership&& value) { SetObjectOwnership(std::move(value)); return *this;}
    ///@}
  private:

    ObjectOwnership m_objectOwnership;
    bool m_objectOwnershipHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
