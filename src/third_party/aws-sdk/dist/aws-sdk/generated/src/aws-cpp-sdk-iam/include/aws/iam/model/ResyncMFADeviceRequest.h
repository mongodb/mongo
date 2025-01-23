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
  class ResyncMFADeviceRequest : public IAMRequest
  {
  public:
    AWS_IAM_API ResyncMFADeviceRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "ResyncMFADevice"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the user whose MFA device you want to resynchronize.</p> <p>This
     * parameter allows (through its <a href="http://wikipedia.org/wiki/regex">regex
     * pattern</a>) a string of characters consisting of upper and lowercase
     * alphanumeric characters with no spaces. You can also include any of the
     * following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetUserName() const{ return m_userName; }
    inline bool UserNameHasBeenSet() const { return m_userNameHasBeenSet; }
    inline void SetUserName(const Aws::String& value) { m_userNameHasBeenSet = true; m_userName = value; }
    inline void SetUserName(Aws::String&& value) { m_userNameHasBeenSet = true; m_userName = std::move(value); }
    inline void SetUserName(const char* value) { m_userNameHasBeenSet = true; m_userName.assign(value); }
    inline ResyncMFADeviceRequest& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline ResyncMFADeviceRequest& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline ResyncMFADeviceRequest& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Serial number that uniquely identifies the MFA device.</p> <p>This parameter
     * allows (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>)
     * a string of characters consisting of upper and lowercase alphanumeric characters
     * with no spaces. You can also include any of the following characters:
     * _+=,.@-</p>
     */
    inline const Aws::String& GetSerialNumber() const{ return m_serialNumber; }
    inline bool SerialNumberHasBeenSet() const { return m_serialNumberHasBeenSet; }
    inline void SetSerialNumber(const Aws::String& value) { m_serialNumberHasBeenSet = true; m_serialNumber = value; }
    inline void SetSerialNumber(Aws::String&& value) { m_serialNumberHasBeenSet = true; m_serialNumber = std::move(value); }
    inline void SetSerialNumber(const char* value) { m_serialNumberHasBeenSet = true; m_serialNumber.assign(value); }
    inline ResyncMFADeviceRequest& WithSerialNumber(const Aws::String& value) { SetSerialNumber(value); return *this;}
    inline ResyncMFADeviceRequest& WithSerialNumber(Aws::String&& value) { SetSerialNumber(std::move(value)); return *this;}
    inline ResyncMFADeviceRequest& WithSerialNumber(const char* value) { SetSerialNumber(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>An authentication code emitted by the device.</p> <p>The format for this
     * parameter is a sequence of six digits.</p>
     */
    inline const Aws::String& GetAuthenticationCode1() const{ return m_authenticationCode1; }
    inline bool AuthenticationCode1HasBeenSet() const { return m_authenticationCode1HasBeenSet; }
    inline void SetAuthenticationCode1(const Aws::String& value) { m_authenticationCode1HasBeenSet = true; m_authenticationCode1 = value; }
    inline void SetAuthenticationCode1(Aws::String&& value) { m_authenticationCode1HasBeenSet = true; m_authenticationCode1 = std::move(value); }
    inline void SetAuthenticationCode1(const char* value) { m_authenticationCode1HasBeenSet = true; m_authenticationCode1.assign(value); }
    inline ResyncMFADeviceRequest& WithAuthenticationCode1(const Aws::String& value) { SetAuthenticationCode1(value); return *this;}
    inline ResyncMFADeviceRequest& WithAuthenticationCode1(Aws::String&& value) { SetAuthenticationCode1(std::move(value)); return *this;}
    inline ResyncMFADeviceRequest& WithAuthenticationCode1(const char* value) { SetAuthenticationCode1(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A subsequent authentication code emitted by the device.</p> <p>The format for
     * this parameter is a sequence of six digits.</p>
     */
    inline const Aws::String& GetAuthenticationCode2() const{ return m_authenticationCode2; }
    inline bool AuthenticationCode2HasBeenSet() const { return m_authenticationCode2HasBeenSet; }
    inline void SetAuthenticationCode2(const Aws::String& value) { m_authenticationCode2HasBeenSet = true; m_authenticationCode2 = value; }
    inline void SetAuthenticationCode2(Aws::String&& value) { m_authenticationCode2HasBeenSet = true; m_authenticationCode2 = std::move(value); }
    inline void SetAuthenticationCode2(const char* value) { m_authenticationCode2HasBeenSet = true; m_authenticationCode2.assign(value); }
    inline ResyncMFADeviceRequest& WithAuthenticationCode2(const Aws::String& value) { SetAuthenticationCode2(value); return *this;}
    inline ResyncMFADeviceRequest& WithAuthenticationCode2(Aws::String&& value) { SetAuthenticationCode2(std::move(value)); return *this;}
    inline ResyncMFADeviceRequest& WithAuthenticationCode2(const char* value) { SetAuthenticationCode2(value); return *this;}
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
