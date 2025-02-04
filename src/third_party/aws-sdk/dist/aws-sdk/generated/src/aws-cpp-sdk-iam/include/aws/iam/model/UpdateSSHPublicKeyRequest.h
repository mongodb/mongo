/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/iam/model/StatusType.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class UpdateSSHPublicKeyRequest : public IAMRequest
  {
  public:
    AWS_IAM_API UpdateSSHPublicKeyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateSSHPublicKey"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the IAM user associated with the SSH public key.</p> <p>This
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
    inline UpdateSSHPublicKeyRequest& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline UpdateSSHPublicKeyRequest& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline UpdateSSHPublicKeyRequest& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The unique identifier for the SSH public key.</p> <p>This parameter allows
     * (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a
     * string of characters that can consist of any upper or lowercased letter or
     * digit.</p>
     */
    inline const Aws::String& GetSSHPublicKeyId() const{ return m_sSHPublicKeyId; }
    inline bool SSHPublicKeyIdHasBeenSet() const { return m_sSHPublicKeyIdHasBeenSet; }
    inline void SetSSHPublicKeyId(const Aws::String& value) { m_sSHPublicKeyIdHasBeenSet = true; m_sSHPublicKeyId = value; }
    inline void SetSSHPublicKeyId(Aws::String&& value) { m_sSHPublicKeyIdHasBeenSet = true; m_sSHPublicKeyId = std::move(value); }
    inline void SetSSHPublicKeyId(const char* value) { m_sSHPublicKeyIdHasBeenSet = true; m_sSHPublicKeyId.assign(value); }
    inline UpdateSSHPublicKeyRequest& WithSSHPublicKeyId(const Aws::String& value) { SetSSHPublicKeyId(value); return *this;}
    inline UpdateSSHPublicKeyRequest& WithSSHPublicKeyId(Aws::String&& value) { SetSSHPublicKeyId(std::move(value)); return *this;}
    inline UpdateSSHPublicKeyRequest& WithSSHPublicKeyId(const char* value) { SetSSHPublicKeyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The status to assign to the SSH public key. <code>Active</code> means that
     * the key can be used for authentication with an CodeCommit repository.
     * <code>Inactive</code> means that the key cannot be used.</p>
     */
    inline const StatusType& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const StatusType& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(StatusType&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline UpdateSSHPublicKeyRequest& WithStatus(const StatusType& value) { SetStatus(value); return *this;}
    inline UpdateSSHPublicKeyRequest& WithStatus(StatusType&& value) { SetStatus(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    Aws::String m_sSHPublicKeyId;
    bool m_sSHPublicKeyIdHasBeenSet = false;

    StatusType m_status;
    bool m_statusHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
