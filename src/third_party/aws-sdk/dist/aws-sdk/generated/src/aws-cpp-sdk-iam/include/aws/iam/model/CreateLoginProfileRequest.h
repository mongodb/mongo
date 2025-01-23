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
  class CreateLoginProfileRequest : public IAMRequest
  {
  public:
    AWS_IAM_API CreateLoginProfileRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "CreateLoginProfile"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the IAM user to create a password for. The user must already
     * exist.</p> <p>This parameter is optional. If no user name is included, it
     * defaults to the principal making the request. When you make this request with
     * root user credentials, you must use an <a
     * href="https://docs.aws.amazon.com/STS/latest/APIReference/API_AssumeRoot.html">AssumeRoot</a>
     * session to omit the user name.</p> <p>This parameter allows (through its <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of characters
     * consisting of upper and lowercase alphanumeric characters with no spaces. You
     * can also include any of the following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetUserName() const{ return m_userName; }
    inline bool UserNameHasBeenSet() const { return m_userNameHasBeenSet; }
    inline void SetUserName(const Aws::String& value) { m_userNameHasBeenSet = true; m_userName = value; }
    inline void SetUserName(Aws::String&& value) { m_userNameHasBeenSet = true; m_userName = std::move(value); }
    inline void SetUserName(const char* value) { m_userNameHasBeenSet = true; m_userName.assign(value); }
    inline CreateLoginProfileRequest& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline CreateLoginProfileRequest& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline CreateLoginProfileRequest& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The new password for the user.</p> <p>This parameter must be omitted when you
     * make the request with an <a
     * href="https://docs.aws.amazon.com/STS/latest/APIReference/API_AssumeRoot.html">AssumeRoot</a>
     * session. It is required in all other cases.</p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> that is used to
     * validate this parameter is a string of characters. That string can include
     * almost any printable ASCII character from the space (<code>\u0020</code>)
     * through the end of the ASCII character range (<code>\u00FF</code>). You can also
     * include the tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>) characters. Any of these characters are
     * valid in a password. However, many tools, such as the Amazon Web Services
     * Management Console, might restrict the ability to type certain characters
     * because they have special meaning within that tool.</p>
     */
    inline const Aws::String& GetPassword() const{ return m_password; }
    inline bool PasswordHasBeenSet() const { return m_passwordHasBeenSet; }
    inline void SetPassword(const Aws::String& value) { m_passwordHasBeenSet = true; m_password = value; }
    inline void SetPassword(Aws::String&& value) { m_passwordHasBeenSet = true; m_password = std::move(value); }
    inline void SetPassword(const char* value) { m_passwordHasBeenSet = true; m_password.assign(value); }
    inline CreateLoginProfileRequest& WithPassword(const Aws::String& value) { SetPassword(value); return *this;}
    inline CreateLoginProfileRequest& WithPassword(Aws::String&& value) { SetPassword(std::move(value)); return *this;}
    inline CreateLoginProfileRequest& WithPassword(const char* value) { SetPassword(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether the user is required to set a new password on next
     * sign-in.</p>
     */
    inline bool GetPasswordResetRequired() const{ return m_passwordResetRequired; }
    inline bool PasswordResetRequiredHasBeenSet() const { return m_passwordResetRequiredHasBeenSet; }
    inline void SetPasswordResetRequired(bool value) { m_passwordResetRequiredHasBeenSet = true; m_passwordResetRequired = value; }
    inline CreateLoginProfileRequest& WithPasswordResetRequired(bool value) { SetPasswordResetRequired(value); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    Aws::String m_password;
    bool m_passwordHasBeenSet = false;

    bool m_passwordResetRequired;
    bool m_passwordResetRequiredHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
