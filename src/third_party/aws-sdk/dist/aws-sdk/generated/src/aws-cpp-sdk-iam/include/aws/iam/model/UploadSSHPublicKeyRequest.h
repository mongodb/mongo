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
  class UploadSSHPublicKeyRequest : public IAMRequest
  {
  public:
    AWS_IAM_API UploadSSHPublicKeyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UploadSSHPublicKey"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the IAM user to associate the SSH public key with.</p> <p>This
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
    inline UploadSSHPublicKeyRequest& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline UploadSSHPublicKeyRequest& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline UploadSSHPublicKeyRequest& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The SSH public key. The public key must be encoded in ssh-rsa format or PEM
     * format. The minimum bit-length of the public key is 2048 bits. For example, you
     * can generate a 2048-bit key, and the resulting PEM file is 1679 bytes long.</p>
     * <p>The <a href="http://wikipedia.org/wiki/regex">regex pattern</a> used to
     * validate this parameter is a string of characters consisting of the
     * following:</p> <ul> <li> <p>Any printable ASCII character ranging from the space
     * character (<code>\u0020</code>) through the end of the ASCII character range</p>
     * </li> <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul>
     */
    inline const Aws::String& GetSSHPublicKeyBody() const{ return m_sSHPublicKeyBody; }
    inline bool SSHPublicKeyBodyHasBeenSet() const { return m_sSHPublicKeyBodyHasBeenSet; }
    inline void SetSSHPublicKeyBody(const Aws::String& value) { m_sSHPublicKeyBodyHasBeenSet = true; m_sSHPublicKeyBody = value; }
    inline void SetSSHPublicKeyBody(Aws::String&& value) { m_sSHPublicKeyBodyHasBeenSet = true; m_sSHPublicKeyBody = std::move(value); }
    inline void SetSSHPublicKeyBody(const char* value) { m_sSHPublicKeyBodyHasBeenSet = true; m_sSHPublicKeyBody.assign(value); }
    inline UploadSSHPublicKeyRequest& WithSSHPublicKeyBody(const Aws::String& value) { SetSSHPublicKeyBody(value); return *this;}
    inline UploadSSHPublicKeyRequest& WithSSHPublicKeyBody(Aws::String&& value) { SetSSHPublicKeyBody(std::move(value)); return *this;}
    inline UploadSSHPublicKeyRequest& WithSSHPublicKeyBody(const char* value) { SetSSHPublicKeyBody(value); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    Aws::String m_sSHPublicKeyBody;
    bool m_sSHPublicKeyBodyHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
