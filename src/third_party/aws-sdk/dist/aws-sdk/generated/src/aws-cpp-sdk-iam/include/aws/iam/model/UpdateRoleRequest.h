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
  class UpdateRoleRequest : public IAMRequest
  {
  public:
    AWS_IAM_API UpdateRoleRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateRole"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the role that you want to modify.</p>
     */
    inline const Aws::String& GetRoleName() const{ return m_roleName; }
    inline bool RoleNameHasBeenSet() const { return m_roleNameHasBeenSet; }
    inline void SetRoleName(const Aws::String& value) { m_roleNameHasBeenSet = true; m_roleName = value; }
    inline void SetRoleName(Aws::String&& value) { m_roleNameHasBeenSet = true; m_roleName = std::move(value); }
    inline void SetRoleName(const char* value) { m_roleNameHasBeenSet = true; m_roleName.assign(value); }
    inline UpdateRoleRequest& WithRoleName(const Aws::String& value) { SetRoleName(value); return *this;}
    inline UpdateRoleRequest& WithRoleName(Aws::String&& value) { SetRoleName(std::move(value)); return *this;}
    inline UpdateRoleRequest& WithRoleName(const char* value) { SetRoleName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The new description that you want to apply to the specified role.</p>
     */
    inline const Aws::String& GetDescription() const{ return m_description; }
    inline bool DescriptionHasBeenSet() const { return m_descriptionHasBeenSet; }
    inline void SetDescription(const Aws::String& value) { m_descriptionHasBeenSet = true; m_description = value; }
    inline void SetDescription(Aws::String&& value) { m_descriptionHasBeenSet = true; m_description = std::move(value); }
    inline void SetDescription(const char* value) { m_descriptionHasBeenSet = true; m_description.assign(value); }
    inline UpdateRoleRequest& WithDescription(const Aws::String& value) { SetDescription(value); return *this;}
    inline UpdateRoleRequest& WithDescription(Aws::String&& value) { SetDescription(std::move(value)); return *this;}
    inline UpdateRoleRequest& WithDescription(const char* value) { SetDescription(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum session duration (in seconds) that you want to set for the
     * specified role. If you do not specify a value for this setting, the default
     * value of one hour is applied. This setting can have a value from 1 hour to 12
     * hours.</p> <p>Anyone who assumes the role from the CLI or API can use the
     * <code>DurationSeconds</code> API parameter or the <code>duration-seconds</code>
     * CLI parameter to request a longer session. The <code>MaxSessionDuration</code>
     * setting determines the maximum duration that can be requested using the
     * <code>DurationSeconds</code> parameter. If users don't specify a value for the
     * <code>DurationSeconds</code> parameter, their security credentials are valid for
     * one hour by default. This applies when you use the <code>AssumeRole*</code> API
     * operations or the <code>assume-role*</code> CLI operations but does not apply
     * when you use those operations to create a console URL. For more information, see
     * <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_roles_use.html">Using
     * IAM roles</a> in the <i>IAM User Guide</i>.</p>  <p>IAM role credentials
     * provided by Amazon EC2 instances assigned to the role are not subject to the
     * specified maximum session duration.</p> 
     */
    inline int GetMaxSessionDuration() const{ return m_maxSessionDuration; }
    inline bool MaxSessionDurationHasBeenSet() const { return m_maxSessionDurationHasBeenSet; }
    inline void SetMaxSessionDuration(int value) { m_maxSessionDurationHasBeenSet = true; m_maxSessionDuration = value; }
    inline UpdateRoleRequest& WithMaxSessionDuration(int value) { SetMaxSessionDuration(value); return *this;}
    ///@}
  private:

    Aws::String m_roleName;
    bool m_roleNameHasBeenSet = false;

    Aws::String m_description;
    bool m_descriptionHasBeenSet = false;

    int m_maxSessionDuration;
    bool m_maxSessionDurationHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
