/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>

namespace Aws
{
namespace Utils
{
namespace Xml
{
  class XmlNode;
} // namespace Xml
} // namespace Utils
namespace IAM
{
namespace Model
{

  /**
   * <p>Contains information about the account password policy.</p> <p> This data
   * type is used as a response element in the <a>GetAccountPasswordPolicy</a>
   * operation. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/PasswordPolicy">AWS
   * API Reference</a></p>
   */
  class PasswordPolicy
  {
  public:
    AWS_IAM_API PasswordPolicy();
    AWS_IAM_API PasswordPolicy(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API PasswordPolicy& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>Minimum length to require for IAM user passwords.</p>
     */
    inline int GetMinimumPasswordLength() const{ return m_minimumPasswordLength; }
    inline bool MinimumPasswordLengthHasBeenSet() const { return m_minimumPasswordLengthHasBeenSet; }
    inline void SetMinimumPasswordLength(int value) { m_minimumPasswordLengthHasBeenSet = true; m_minimumPasswordLength = value; }
    inline PasswordPolicy& WithMinimumPasswordLength(int value) { SetMinimumPasswordLength(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether IAM user passwords must contain at least one of the
     * following symbols:</p> <p>! @ # $ % ^ &amp; * ( ) _ + - = [ ] { } | '</p>
     */
    inline bool GetRequireSymbols() const{ return m_requireSymbols; }
    inline bool RequireSymbolsHasBeenSet() const { return m_requireSymbolsHasBeenSet; }
    inline void SetRequireSymbols(bool value) { m_requireSymbolsHasBeenSet = true; m_requireSymbols = value; }
    inline PasswordPolicy& WithRequireSymbols(bool value) { SetRequireSymbols(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether IAM user passwords must contain at least one numeric
     * character (0 to 9).</p>
     */
    inline bool GetRequireNumbers() const{ return m_requireNumbers; }
    inline bool RequireNumbersHasBeenSet() const { return m_requireNumbersHasBeenSet; }
    inline void SetRequireNumbers(bool value) { m_requireNumbersHasBeenSet = true; m_requireNumbers = value; }
    inline PasswordPolicy& WithRequireNumbers(bool value) { SetRequireNumbers(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether IAM user passwords must contain at least one uppercase
     * character (A to Z).</p>
     */
    inline bool GetRequireUppercaseCharacters() const{ return m_requireUppercaseCharacters; }
    inline bool RequireUppercaseCharactersHasBeenSet() const { return m_requireUppercaseCharactersHasBeenSet; }
    inline void SetRequireUppercaseCharacters(bool value) { m_requireUppercaseCharactersHasBeenSet = true; m_requireUppercaseCharacters = value; }
    inline PasswordPolicy& WithRequireUppercaseCharacters(bool value) { SetRequireUppercaseCharacters(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether IAM user passwords must contain at least one lowercase
     * character (a to z).</p>
     */
    inline bool GetRequireLowercaseCharacters() const{ return m_requireLowercaseCharacters; }
    inline bool RequireLowercaseCharactersHasBeenSet() const { return m_requireLowercaseCharactersHasBeenSet; }
    inline void SetRequireLowercaseCharacters(bool value) { m_requireLowercaseCharactersHasBeenSet = true; m_requireLowercaseCharacters = value; }
    inline PasswordPolicy& WithRequireLowercaseCharacters(bool value) { SetRequireLowercaseCharacters(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether IAM users are allowed to change their own password. Gives
     * IAM users permissions to <code>iam:ChangePassword</code> for only their user and
     * to the <code>iam:GetAccountPasswordPolicy</code> action. This option does not
     * attach a permissions policy to each user, rather the permissions are applied at
     * the account-level for all users by IAM.</p>
     */
    inline bool GetAllowUsersToChangePassword() const{ return m_allowUsersToChangePassword; }
    inline bool AllowUsersToChangePasswordHasBeenSet() const { return m_allowUsersToChangePasswordHasBeenSet; }
    inline void SetAllowUsersToChangePassword(bool value) { m_allowUsersToChangePasswordHasBeenSet = true; m_allowUsersToChangePassword = value; }
    inline PasswordPolicy& WithAllowUsersToChangePassword(bool value) { SetAllowUsersToChangePassword(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Indicates whether passwords in the account expire. Returns true if
     * <code>MaxPasswordAge</code> contains a value greater than 0. Returns false if
     * MaxPasswordAge is 0 or not present.</p>
     */
    inline bool GetExpirePasswords() const{ return m_expirePasswords; }
    inline bool ExpirePasswordsHasBeenSet() const { return m_expirePasswordsHasBeenSet; }
    inline void SetExpirePasswords(bool value) { m_expirePasswordsHasBeenSet = true; m_expirePasswords = value; }
    inline PasswordPolicy& WithExpirePasswords(bool value) { SetExpirePasswords(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The number of days that an IAM user password is valid.</p>
     */
    inline int GetMaxPasswordAge() const{ return m_maxPasswordAge; }
    inline bool MaxPasswordAgeHasBeenSet() const { return m_maxPasswordAgeHasBeenSet; }
    inline void SetMaxPasswordAge(int value) { m_maxPasswordAgeHasBeenSet = true; m_maxPasswordAge = value; }
    inline PasswordPolicy& WithMaxPasswordAge(int value) { SetMaxPasswordAge(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the number of previous passwords that IAM users are prevented from
     * reusing.</p>
     */
    inline int GetPasswordReusePrevention() const{ return m_passwordReusePrevention; }
    inline bool PasswordReusePreventionHasBeenSet() const { return m_passwordReusePreventionHasBeenSet; }
    inline void SetPasswordReusePrevention(int value) { m_passwordReusePreventionHasBeenSet = true; m_passwordReusePrevention = value; }
    inline PasswordPolicy& WithPasswordReusePrevention(int value) { SetPasswordReusePrevention(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies whether IAM users are prevented from setting a new password via the
     * Amazon Web Services Management Console after their password has expired. The IAM
     * user cannot access the console until an administrator resets the password. IAM
     * users with <code>iam:ChangePassword</code> permission and active access keys can
     * reset their own expired console password using the CLI or API.</p>
     */
    inline bool GetHardExpiry() const{ return m_hardExpiry; }
    inline bool HardExpiryHasBeenSet() const { return m_hardExpiryHasBeenSet; }
    inline void SetHardExpiry(bool value) { m_hardExpiryHasBeenSet = true; m_hardExpiry = value; }
    inline PasswordPolicy& WithHardExpiry(bool value) { SetHardExpiry(value); return *this;}
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

    bool m_expirePasswords;
    bool m_expirePasswordsHasBeenSet = false;

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
