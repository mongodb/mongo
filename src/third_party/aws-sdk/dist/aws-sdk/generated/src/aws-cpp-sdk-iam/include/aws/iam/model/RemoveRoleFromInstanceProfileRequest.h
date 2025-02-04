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
  class RemoveRoleFromInstanceProfileRequest : public IAMRequest
  {
  public:
    AWS_IAM_API RemoveRoleFromInstanceProfileRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "RemoveRoleFromInstanceProfile"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the instance profile to update.</p> <p>This parameter allows
     * (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a
     * string of characters consisting of upper and lowercase alphanumeric characters
     * with no spaces. You can also include any of the following characters:
     * _+=,.@-</p>
     */
    inline const Aws::String& GetInstanceProfileName() const{ return m_instanceProfileName; }
    inline bool InstanceProfileNameHasBeenSet() const { return m_instanceProfileNameHasBeenSet; }
    inline void SetInstanceProfileName(const Aws::String& value) { m_instanceProfileNameHasBeenSet = true; m_instanceProfileName = value; }
    inline void SetInstanceProfileName(Aws::String&& value) { m_instanceProfileNameHasBeenSet = true; m_instanceProfileName = std::move(value); }
    inline void SetInstanceProfileName(const char* value) { m_instanceProfileNameHasBeenSet = true; m_instanceProfileName.assign(value); }
    inline RemoveRoleFromInstanceProfileRequest& WithInstanceProfileName(const Aws::String& value) { SetInstanceProfileName(value); return *this;}
    inline RemoveRoleFromInstanceProfileRequest& WithInstanceProfileName(Aws::String&& value) { SetInstanceProfileName(std::move(value)); return *this;}
    inline RemoveRoleFromInstanceProfileRequest& WithInstanceProfileName(const char* value) { SetInstanceProfileName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the role to remove.</p> <p>This parameter allows (through its <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of characters
     * consisting of upper and lowercase alphanumeric characters with no spaces. You
     * can also include any of the following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetRoleName() const{ return m_roleName; }
    inline bool RoleNameHasBeenSet() const { return m_roleNameHasBeenSet; }
    inline void SetRoleName(const Aws::String& value) { m_roleNameHasBeenSet = true; m_roleName = value; }
    inline void SetRoleName(Aws::String&& value) { m_roleNameHasBeenSet = true; m_roleName = std::move(value); }
    inline void SetRoleName(const char* value) { m_roleNameHasBeenSet = true; m_roleName.assign(value); }
    inline RemoveRoleFromInstanceProfileRequest& WithRoleName(const Aws::String& value) { SetRoleName(value); return *this;}
    inline RemoveRoleFromInstanceProfileRequest& WithRoleName(Aws::String&& value) { SetRoleName(std::move(value)); return *this;}
    inline RemoveRoleFromInstanceProfileRequest& WithRoleName(const char* value) { SetRoleName(value); return *this;}
    ///@}
  private:

    Aws::String m_instanceProfileName;
    bool m_instanceProfileNameHasBeenSet = false;

    Aws::String m_roleName;
    bool m_roleNameHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
