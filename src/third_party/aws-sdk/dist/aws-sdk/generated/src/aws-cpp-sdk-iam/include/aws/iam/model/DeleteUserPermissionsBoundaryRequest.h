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
  class DeleteUserPermissionsBoundaryRequest : public IAMRequest
  {
  public:
    AWS_IAM_API DeleteUserPermissionsBoundaryRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "DeleteUserPermissionsBoundary"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name (friendly name, not ARN) of the IAM user from which you want to
     * remove the permissions boundary.</p>
     */
    inline const Aws::String& GetUserName() const{ return m_userName; }
    inline bool UserNameHasBeenSet() const { return m_userNameHasBeenSet; }
    inline void SetUserName(const Aws::String& value) { m_userNameHasBeenSet = true; m_userName = value; }
    inline void SetUserName(Aws::String&& value) { m_userNameHasBeenSet = true; m_userName = std::move(value); }
    inline void SetUserName(const char* value) { m_userNameHasBeenSet = true; m_userName.assign(value); }
    inline DeleteUserPermissionsBoundaryRequest& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline DeleteUserPermissionsBoundaryRequest& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline DeleteUserPermissionsBoundaryRequest& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
