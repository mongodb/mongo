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
  class UpdateLoginProfileRequest : public IAMRequest
  {
  public:
    AWS_IAM_API UpdateLoginProfileRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateLoginProfile"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the user whose password you want to update.</p> <p>This parameter
     * allows (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>)
     * a string of characters consisting of upper and lowercase alphanumeric characters
     * with no spaces. You can also include any of the following characters:
     * _+=,.@-</p>
     */
    inline const Aws::String& GetUserName() const{ return m_userName; }
    inline bool UserNameHasBeenSet() const { return m_userNameHasBeenSet; }
    inline void SetUserName(const Aws::String& value) { m_userNameHasBeenSet = true; m_userName = value; }
    inline void SetUserName(Aws::String&& value) { m_userNameHasBeenSet = true; m_userName = std::move(value); }
    inline void SetUserName(const char* value) { m_userNameHasBeenSet = true; m_userName.assign(value); }
    inline UpdateLoginProfileRequest& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline UpdateLoginProfileRequest& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline UpdateLoginProfileRequest& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The new password for the specified IAM user.</p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate this
     * parameter is a string of characters consisting of the following:</p> <ul> <li>
     * <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul> <p>However, the format can
     * be further restricted by the account administrator by setting a password policy
     * on the Amazon Web Services account. For more information, see
     * <a>UpdateAccountPasswordPolicy</a>.</p>
     */
    inline const Aws::String& GetPassword() const{ return m_password; }
    inline bool PasswordHasBeenSet() const { return m_passwordHasBeenSet; }
    inline void SetPassword(const Aws::String& value) { m_passwordHasBeenSet = true; m_password = value; }
    inline void SetPassword(Aws::String&& value) { m_passwordHasBeenSet = true; m_password = std::move(value); }
    inline void SetPassword(const char* value) { m_passwordHasBeenSet = true; m_password.assign(value); }
    inline UpdateLoginProfileRequest& WithPassword(const Aws::String& value) { SetPassword(value); return *this;}
    inline UpdateLoginProfileRequest& WithPassword(Aws::String&& value) { SetPassword(std::move(value)); return *this;}
    inline UpdateLoginProfileRequest& WithPassword(const char* value) { SetPassword(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Allows this new password to be used only once by requiring the specified IAM
     * user to set a new password on next sign-in.</p>
     */
    inline bool GetPasswordResetRequired() const{ return m_passwordResetRequired; }
    inline bool PasswordResetRequiredHasBeenSet() const { return m_passwordResetRequiredHasBeenSet; }
    inline void SetPasswordResetRequired(bool value) { m_passwordResetRequiredHasBeenSet = true; m_passwordResetRequired = value; }
    inline UpdateLoginProfileRequest& WithPasswordResetRequired(bool value) { SetPasswordResetRequired(value); return *this;}
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
