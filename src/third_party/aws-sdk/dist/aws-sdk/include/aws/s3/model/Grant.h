/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/Grantee.h>
#include <aws/s3/model/Permission.h>
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
   * <p>Container for grant information.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/s3-2006-03-01/Grant">AWS API
   * Reference</a></p>
   */
  class Grant
  {
  public:
    AWS_S3_API Grant();
    AWS_S3_API Grant(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_S3_API Grant& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_S3_API void AddToNode(Aws::Utils::Xml::XmlNode& parentNode) const;


    ///@{
    /**
     * <p>The person being granted permissions.</p>
     */
    inline const Grantee& GetGrantee() const{ return m_grantee; }
    inline bool GranteeHasBeenSet() const { return m_granteeHasBeenSet; }
    inline void SetGrantee(const Grantee& value) { m_granteeHasBeenSet = true; m_grantee = value; }
    inline void SetGrantee(Grantee&& value) { m_granteeHasBeenSet = true; m_grantee = std::move(value); }
    inline Grant& WithGrantee(const Grantee& value) { SetGrantee(value); return *this;}
    inline Grant& WithGrantee(Grantee&& value) { SetGrantee(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the permission given to the grantee.</p>
     */
    inline const Permission& GetPermission() const{ return m_permission; }
    inline bool PermissionHasBeenSet() const { return m_permissionHasBeenSet; }
    inline void SetPermission(const Permission& value) { m_permissionHasBeenSet = true; m_permission = value; }
    inline void SetPermission(Permission&& value) { m_permissionHasBeenSet = true; m_permission = std::move(value); }
    inline Grant& WithPermission(const Permission& value) { SetPermission(value); return *this;}
    inline Grant& WithPermission(Permission&& value) { SetPermission(std::move(value)); return *this;}
    ///@}
  private:

    Grantee m_grantee;
    bool m_granteeHasBeenSet = false;

    Permission m_permission;
    bool m_permissionHasBeenSet = false;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
