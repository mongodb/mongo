/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class UpdateAccountPasswordPolicyRequest : public IAMRequest
  {
  public:
    AWS_IAM_API UpdateAccountPasswordPolicyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateAccountPasswordPolicy"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The minimum number of characters allowed in an IAM user password.</p> <p>If
     * you do not specify a value for this parameter, then the operation uses the
     * default value of <code>6</code>.</p>
     */
    inline int GetMinimumPasswordLength() const{ return m_minimumPasswordLength; }
    inline bool MinimumPasswordLengthHasBeenSet() const { return m_minimumPasswordLengthHasBeenSet; }
    inline void SetMinimumPasswordLength(int value) { m_minimumPasswordLengthHasBeenSet = true; m_minimumPasswordLength = value; }
    inline UpdateAccountPasswordPolicyRequest& WithMinimumPasswordLength(int value) { SetMinimumPasswordLength(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether IAM user passwords must contain at least one of the
     * following non-alphanumeric characters:</p> <p>! @ # $ % ^ &amp; * ( ) _ + - = [
     * ] { } | '</p> <p>If you do not specify a value for this parameter, then the
     * operation uses the default value of <code>false</code>. The result is that
     * passwords do not require at least one symbol character.</p>
     */
    inline bool GetRequireSymbols() const{ return m_requireSymbols; }
    inline bool RequireSymbolsHasBeenSet() const { return m_requireSymbolsHasBeenSet; }
    inline void SetRequireSymbols(bool value) { m_requireSymbolsHasBeenSet = true; m_requireSymbols = value; }
    inline UpdateAccountPasswordPolicyRequest& WithRequireSymbols(bool value) { SetRequireSymbols(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether IAM user passwords must contain at least one numeric
     * character (0 to 9).</p> <p>If you do not specify a value for this parameter,
     * then the operation uses the default value of <code>false</code>. The result is
     * that passwords do not require at least one numeric character.</p>
     */
    inline bool GetRequireNumbers() const{ return m_requireNumbers; }
    inline bool RequireNumbersHasBeenSet() const { return m_requireNumbersHasBeenSet; }
    inline void SetRequireNumbers(bool value) { m_requireNumbersHasBeenSet = true; m_requireNumbers = value; }
    inline UpdateAccountPasswordPolicyRequest& WithRequireNumbers(bool value) { SetRequireNumbers(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether IAM user passwords must contain at least one uppercase
     * character from the ISO basic Latin alphabet (A to Z).</p> <p>If you do not
     * specify a value for this parameter, then the operation uses the default value of
     * <code>false</code>. The result is that passwords do not require at least one
     * uppercase character.</p>
     */
    inline bool GetRequireUppercaseCharacters() const{ return m_requireUppercaseCharacters; }
    inline bool RequireUppercaseCharactersHasBeenSet() const { return m_requireUppercaseCharactersHasBeenSet; }
    inline void SetRequireUppercaseCharacters(bool value) { m_requireUppercaseCharactersHasBeenSet = true; m_requireUppercaseCharacters = value; }
    inline UpdateAccountPasswordPolicyRequest& WithRequireUppercaseCharacters(bool value) { SetRequireUppercaseCharacters(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether IAM user passwords must contain at least one lowercase
     * character from the ISO basic Latin alphabet (a to z).</p> <p>If you do not
     * specify a value for this parameter, then the operation uses the default value of
     * <code>false</code>. The result is that passwords do not require at least one
     * lowercase character.</p>
     */
    inline bool GetRequireLowercaseCharacters() const{ return m_requireLowercaseCharacters; }
    inline bool RequireLowercaseCharactersHasBeenSet() const { return m_requireLowercaseCharactersHasBeenSet; }
    inline void SetRequireLowercaseCharacters(bool value) { m_requireLowercaseCharactersHasBeenSet = true; m_requireLowercaseCharacters = value; }
    inline UpdateAccountPasswordPolicyRequest& WithRequireLowercaseCharacters(bool value) { SetRequireLowercaseCharacters(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> Allows all IAM users in your account to use the Amazon Web Services
     * Management Console to change their own passwords. For more information, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_credentials_passwords_enable-user-change.html">Permitting
     * IAM users to change their own passwords</a> in the <i>IAM User Guide</i>.</p>
     * <p>If you do not specify a value for this parameter, then the operation uses the
     * default value of <code>false</code>. The result is that IAM users in the account
     * do not automatically have permissions to change their own password.</p>
     */
    inline bool GetAllowUsersToChangePassword() const{ return m_allowUsersToChangePassword; }
    inline bool AllowUsersToChangePasswordHasBeenSet() const { return m_allowUsersToChangePasswordHasBeenSet; }
    inline void SetAllowUsersToChangePassword(bool value) { m_allowUsersToChangePasswordHasBeenSet = true; m_allowUsersToChangePassword = value; }
    inline UpdateAccountPasswordPolicyRequest& WithAllowUsersToChangePassword(bool value) { SetAllowUsersToChangePassword(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The number of days that an IAM user password is valid.</p> <p>If you do not
     * specify a value for this parameter, then the operation uses the default value of
     * <code>0</code>. The result is that IAM user passwords never expire.</p>
     */
    inline int GetMaxPasswordAge() const{ return m_maxPasswordAge; }
    inline bool MaxPasswordAgeHasBeenSet() const { return m_maxPasswordAgeHasBeenSet; }
    inline void SetMaxPasswordAge(int value) { m_maxPasswordAgeHasBeenSet = true; m_maxPasswordAge = value; }
    inline UpdateAccountPasswordPolicyRequest& WithMaxPasswordAge(int value) { SetMaxPasswordAge(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the number of previous passwords that IAM users are prevented from
     * reusing.</p> <p>If you do not specify a value for this parameter, then the
     * operation uses the default value of <code>0</code>. The result is that IAM users
     * are not prevented from reusing previous passwords.</p>
     */
    inline int GetPasswordReusePrevention() const{ return m_passwordReusePrevention; }
    inline bool PasswordReusePreventionHasBeenSet() const { return m_passwordReusePreventionHasBeenSet; }
    inline void SetPasswordReusePrevention(int value) { m_passwordReusePreventionHasBeenSet = true; m_passwordReusePrevention = value; }
    inline UpdateAccountPasswordPolicyRequest& WithPasswordReusePrevention(int value) { SetPasswordReusePrevention(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> Prevents IAM users who are accessing the account via the Amazon Web Services
     * Management Console from setting a new console password after their password has
     * expired. The IAM user cannot access the console until an administrator resets
     * the password.</p> <p>If you do not specify a value for this parameter, then the
     * operation uses the default value of <code>false</code>. The result is that IAM
     * users can change their passwords after they expire and continue to sign in as
     * the user.</p>  <p> In the Amazon Web Services Management Console, the
     * custom password policy option <b>Allow users to change their own password</b>
     * gives IAM users permissions to <code>iam:ChangePassword</code> for only their
     * user and to the <code>iam:GetAccountPasswordPolicy</code> action. This option
     * does not attach a permissions policy to each user, rather the permissions are
     * applied at the account-level for all users by IAM. IAM users with
     * <code>iam:ChangePassword</code> permission and active access keys can reset
     * their own expired console password using the CLI or API.</p> 
     */
    inline bool GetHardExpiry() const{ return m_hardExpiry; }
    inline bool HardExpiryHasBeenSet() const { return m_hardExpiryHasBeenSet; }
    inline void SetHardExpiry(bool value) { m_hardExpiryHasBeenSet = true; m_hardExpiry = value; }
    inline UpdateAccountPasswordPolicyRequest& WithHardExpiry(bool value) { SetHardExpiry(value); return *this;}
    ///@}
  private:

    int m_minimumPasswordLength;
    bool m_minimumPasswordLengthHasBeenSet = false;

    bool m_requireSymbols;
    bool m_requireSymbolsHasBeenSet = false;

    bool m_requireNumbers;
    bool m_requireNumbersHasBeenSet = false;

    bool m_requireUppercaseCharacters;
    bool m_requireUppercaseCharactersHasBeenSet = false;

    bool m_requireLowercaseCharacters;
    bool m_requireLowercaseCharactersHasBeenSet = false;

    bool m_allowUsersToChangePassword;
    bool m_allowUsersToChangePasswordHasBeenSet = false;

    int m_maxPasswordAge;
    bool m_maxPasswordAgeHasBeenSet = false;

    int m_passwordReusePrevention;
    bool m_passwordReusePreventionHasBeenSet = false;

    bool m_hardExpiry;
    bool m_hardExpiryHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
