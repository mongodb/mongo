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
  class GetRolePolicyRequest : public IAMRequest
  {
  public:
    AWS_IAM_API GetRolePolicyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "GetRolePolicy"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the role associated with the policy.</p> <p>This parameter allows
     * (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a
     * string of characters consisting of upper and lowercase alphanumeric characters
     * with no spaces. You can also include any of the following characters:
     * _+=,.@-</p>
     */
    inline const Aws::String& GetRoleName() const{ return m_roleName; }
    inline bool RoleNameHasBeenSet() const { return m_roleNameHasBeenSet; }
    inline void SetRoleName(const Aws::String& value) { m_roleNameHasBeenSet = true; m_roleName = value; }
    inline void SetRoleName(Aws::String&& value) { m_roleNameHasBeenSet = true; m_roleName = std::move(value); }
    inline void SetRoleName(const char* value) { m_roleNameHasBeenSet = true; m_roleName.assign(value); }
    inline GetRolePolicyRequest& WithRoleName(const Aws::String& value) { SetRoleName(value); return *this;}
    inline GetRolePolicyRequest& WithRoleName(Aws::String&& value) { SetRoleName(std::move(value)); return *this;}
    inline GetRolePolicyRequest& WithRoleName(const char* value) { SetRoleName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name of the policy document to get.</p> <p>This parameter allows (through
     * its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of
     * characters consisting of upper and lowercase alphanumeric characters with no
     * spaces. You can also include any of the following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetPolicyName() const{ return m_policyName; }
    inline bool PolicyNameHasBeenSet() const { return m_policyNameHasBeenSet; }
    inline void SetPolicyName(const Aws::String& value) { m_policyNameHasBeenSet = true; m_policyName = value; }
    inline void SetPolicyName(Aws::String&& value) { m_policyNameHasBeenSet = true; m_policyName = std::move(value); }
    inline void SetPolicyName(const char* value) { m_policyNameHasBeenSet = true; m_policyName.assign(value); }
    inline GetRolePolicyRequest& WithPolicyName(const Aws::String& value) { SetPolicyName(value); return *this;}
    inline GetRolePolicyRequest& WithPolicyName(Aws::String&& value) { SetPolicyName(std::move(value)); return *this;}
    inline GetRolePolicyRequest& WithPolicyName(const char* value) { SetPolicyName(value); return *this;}
    ///@}
  private:

    Aws::String m_roleName;
    bool m_roleNameHasBeenSet = false;

    Aws::String m_policyName;
    bool m_policyNameHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
