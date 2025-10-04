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
  class EnableMFADeviceRequest : public IAMRequest
  {
  public:
    AWS_IAM_API EnableMFADeviceRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "EnableMFADevice"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the IAM user for whom you want to enable the MFA device.</p>
     * <p>This parameter allows (through its <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of characters
     * consisting of upper and lowercase alphanumeric characters with no spaces. You
     * can also include any of the following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetUserName() const{ return m_userName; }
    inline bool UserNameHasBeenSet() const { return m_userNameHasBeenSet; }
    inline void SetUserName(const Aws::String& value) { m_userNameHasBeenSet = true; m_userName = value; }
    inline void SetUserName(Aws::String&& value) { m_userNameHasBeenSet = true; m_userName = std::move(value); }
    inline void SetUserName(const char* value) { m_userNameHasBeenSet = true; m_userName.assign(value); }
    inline EnableMFADeviceRequest& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline EnableMFADeviceRequest& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline EnableMFADeviceRequest& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The serial number that uniquely identifies the MFA device. For virtual MFA
     * devices, the serial number is the device ARN.</p> <p>This parameter allows
     * (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a
     * string of characters consisting of upper and lowercase alphanumeric characters
     * with no spaces. You can also include any of the following characters:
     * =,.@:/-</p>
     */
    inline const Aws::String& GetSerialNumber() const{ return m_serialNumber; }
    inline bool SerialNumberHasBeenSet() const { return m_serialNumberHasBeenSet; }
    inline void SetSerialNumber(const Aws::String& value) { m_serialNumberHasBeenSet = true; m_serialNumber = value; }
    inline void SetSerialNumber(Aws::String&& value) { m_serialNumberHasBeenSet = true; m_serialNumber = std::move(value); }
    inline void SetSerialNumber(const char* value) { m_serialNumberHasBeenSet = true; m_serialNumber.assign(value); }
    inline EnableMFADeviceRequest& WithSerialNumber(const Aws::String& value) { SetSerialNumber(value); return *this;}
    inline EnableMFADeviceRequest& WithSerialNumber(Aws::String&& value) { SetSerialNumber(std::move(value)); return *this;}
    inline EnableMFADeviceRequest& WithSerialNumber(const char* value) { SetSerialNumber(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An authentication code emitted by the device. </p> <p>The format for this
     * parameter is a string of six digits.</p>  <p>Submit your request
     * immediately after generating the authentication codes. If you generate the codes
     * and then wait too long to submit the request, the MFA device successfully
     * associates with the user but the MFA device becomes out of sync. This happens
     * because time-based one-time passwords (TOTP) expire after a short period of
     * time. If this happens, you can <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_credentials_mfa_sync.html">resync
     * the device</a>.</p> 
     */
    inline const Aws::String& GetAuthenticationCode1() const{ return m_authenticationCode1; }
    inline bool AuthenticationCode1HasBeenSet() const { return m_authenticationCode1HasBeenSet; }
    inline void SetAuthenticationCode1(const Aws::String& value) { m_authenticationCode1HasBeenSet = true; m_authenticationCode1 = value; }
    inline void SetAuthenticationCode1(Aws::String&& value) { m_authenticationCode1HasBeenSet = true; m_authenticationCode1 = std::move(value); }
    inline void SetAuthenticationCode1(const char* value) { m_authenticationCode1HasBeenSet = true; m_authenticationCode1.assign(value); }
    inline EnableMFADeviceRequest& WithAuthenticationCode1(const Aws::String& value) { SetAuthenticationCode1(value); return *this;}
    inline EnableMFADeviceRequest& WithAuthenticationCode1(Aws::String&& value) { SetAuthenticationCode1(std::move(value)); return *this;}
    inline EnableMFADeviceRequest& WithAuthenticationCode1(const char* value) { SetAuthenticationCode1(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A subsequent authentication code emitted by the device.</p> <p>The format for
     * this parameter is a string of six digits.</p>  <p>Submit your request
     * immediately after generating the authentication codes. If you generate the codes
     * and then wait too long to submit the request, the MFA device successfully
     * associates with the user but the MFA device becomes out of sync. This happens
     * because time-based one-time passwords (TOTP) expire after a short period of
     * time. If this happens, you can <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_credentials_mfa_sync.html">resync
     * the device</a>.</p> 
     */
    inline const Aws::String& GetAuthenticationCode2() const{ return m_authenticationCode2; }
    inline bool AuthenticationCode2HasBeenSet() const { return m_authenticationCode2HasBeenSet; }
    inline void SetAuthenticationCode2(const Aws::String& value) { m_authenticationCode2HasBeenSet = true; m_authenticationCode2 = value; }
    inline void SetAuthenticationCode2(Aws::String&& value) { m_authenticationCode2HasBeenSet = true; m_authenticationCode2 = std::move(value); }
    inline void SetAuthenticationCode2(const char* value) { m_authenticationCode2HasBeenSet = true; m_authenticationCode2.assign(value); }
    inline EnableMFADeviceRequest& WithAuthenticationCode2(const Aws::String& value) { SetAuthenticationCode2(value); return *this;}
    inline EnableMFADeviceRequest& WithAuthenticationCode2(Aws::String&& value) { SetAuthenticationCode2(std::move(value)); return *this;}
    inline EnableMFADeviceRequest& WithAuthenticationCode2(const char* value) { SetAuthenticationCode2(value); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    Aws::String m_serialNumber;
    bool m_serialNumberHasBeenSet = false;

    Aws::String m_authenticationCode1;
    bool m_authenticationCode1HasBeenSet = false;

    Aws::String m_authenticationCode2;
    bool m_authenticationCode2HasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
