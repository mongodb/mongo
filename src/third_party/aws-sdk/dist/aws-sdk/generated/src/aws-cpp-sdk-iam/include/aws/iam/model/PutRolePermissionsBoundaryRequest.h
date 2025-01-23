/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class PutRolePermissionsBoundaryRequest : public IAMRequest
  {
  public:
    AWS_IAM_API PutRolePermissionsBoundaryRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "PutRolePermissionsBoundary"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name (friendly name, not ARN) of the IAM role for which you want to set
     * the permissions boundary.</p>
     */
    inline const Aws::String& GetRoleName() const{ return m_roleName; }
    inline bool RoleNameHasBeenSet() const { return m_roleNameHasBeenSet; }
    inline void SetRoleName(const Aws::String& value) { m_roleNameHasBeenSet = true; m_roleName = value; }
    inline void SetRoleName(Aws::String&& value) { m_roleNameHasBeenSet = true; m_roleName = std::move(value); }
    inline void SetRoleName(const char* value) { m_roleNameHasBeenSet = true; m_roleName.assign(value); }
    inline PutRolePermissionsBoundaryRequest& WithRoleName(const Aws::String& value) { SetRoleName(value); return *this;}
    inline PutRolePermissionsBoundaryRequest& WithRoleName(Aws::String&& value) { SetRoleName(std::move(value)); return *this;}
    inline PutRolePermissionsBoundaryRequest& WithRoleName(const char* value) { SetRoleName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ARN of the managed policy that is used to set the permissions boundary
     * for the role.</p> <p>A permissions boundary policy defines the maximum
     * permissions that identity-based policies can grant to an entity, but does not
     * grant permissions. Permissions boundaries do not define the maximum permissions
     * that a resource-based policy can grant to an entity. To learn more, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies_boundaries.html">Permissions
     * boundaries for IAM entities</a> in the <i>IAM User Guide</i>.</p> <p>For more
     * information about policy types, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/access_policies.html#access_policy-types">Policy
     * types </a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::String& GetPermissionsBoundary() const{ return m_permissionsBoundary; }
    inline bool PermissionsBoundaryHasBeenSet() const { return m_permissionsBoundaryHasBeenSet; }
    inline void SetPermissionsBoundary(const Aws::String& value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary = value; }
    inline void SetPermissionsBoundary(Aws::String&& value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary = std::move(value); }
    inline void SetPermissionsBoundary(const char* value) { m_permissionsBoundaryHasBeenSet = true; m_permissionsBoundary.assign(value); }
    inline PutRolePermissionsBoundaryRequest& WithPermissionsBoundary(const Aws::String& value) { SetPermissionsBoundary(value); return *this;}
    inline PutRolePermissionsBoundaryRequest& WithPermissionsBoundary(Aws::String&& value) { SetPermissionsBoundary(std::move(value)); return *this;}
    inline PutRolePermissionsBoundaryRequest& WithPermissionsBoundary(const char* value) { SetPermissionsBoundary(value); return *this;}
    ///@}
  private:

    Aws::String m_roleName;
    bool m_roleNameHasBeenSet = false;

    Aws::String m_permissionsBoundary;
    bool m_permissionsBoundaryHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
