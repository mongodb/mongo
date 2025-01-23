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
  class DeactivateMFADeviceRequest : public IAMRequest
  {
  public:
    AWS_IAM_API DeactivateMFADeviceRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "DeactivateMFADevice"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the user whose MFA device you want to deactivate.</p> <p>This
     * parameter is optional. If no user name is included, it defaults to the principal
     * making the request. When you make this request with root user credentials, you
     * must use an <a
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
    inline DeactivateMFADeviceRequest& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline DeactivateMFADeviceRequest& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline DeactivateMFADeviceRequest& WithUserName(const char* value) { SetUserName(value); return *this;}
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
    inline DeactivateMFADeviceRequest& WithSerialNumber(const Aws::String& value) { SetSerialNumber(value); return *this;}
    inline DeactivateMFADeviceRequest& WithSerialNumber(Aws::String&& value) { SetSerialNumber(std::move(value)); return *this;}
    inline DeactivateMFADeviceRequest& WithSerialNumber(const char* value) { SetSerialNumber(value); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    Aws::String m_serialNumber;
    bool m_serialNumberHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
