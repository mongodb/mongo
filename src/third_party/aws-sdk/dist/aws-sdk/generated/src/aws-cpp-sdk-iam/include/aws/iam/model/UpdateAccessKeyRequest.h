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
  class UpdateAccessKeyRequest : public IAMRequest
  {
  public:
    AWS_IAM_API UpdateAccessKeyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateAccessKey"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the user whose key you want to update.</p> <p>This parameter
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
    inline UpdateAccessKeyRequest& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline UpdateAccessKeyRequest& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline UpdateAccessKeyRequest& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The access key ID of the secret access key you want to update.</p> <p>This
     * parameter allows (through its <a href="http://wikipedia.org/wiki/regex">regex
     * pattern</a>) a string of characters that can consist of any upper or lowercased
     * letter or digit.</p>
     */
    inline const Aws::String& GetAccessKeyId() const{ return m_accessKeyId; }
    inline bool AccessKeyIdHasBeenSet() const { return m_accessKeyIdHasBeenSet; }
    inline void SetAccessKeyId(const Aws::String& value) { m_accessKeyIdHasBeenSet = true; m_accessKeyId = value; }
    inline void SetAccessKeyId(Aws::String&& value) { m_accessKeyIdHasBeenSet = true; m_accessKeyId = std::move(value); }
    inline void SetAccessKeyId(const char* value) { m_accessKeyIdHasBeenSet = true; m_accessKeyId.assign(value); }
    inline UpdateAccessKeyRequest& WithAccessKeyId(const Aws::String& value) { SetAccessKeyId(value); return *this;}
    inline UpdateAccessKeyRequest& WithAccessKeyId(Aws::String&& value) { SetAccessKeyId(std::move(value)); return *this;}
    inline UpdateAccessKeyRequest& WithAccessKeyId(const char* value) { SetAccessKeyId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> The status you want to assign to the secret access key. <code>Active</code>
     * means that the key can be used for programmatic calls to Amazon Web Services,
     * while <code>Inactive</code> means that the key cannot be used.</p>
     */
    inline const StatusType& GetStatus() const{ return m_status; }
    inline bool StatusHasBeenSet() const { return m_statusHasBeenSet; }
    inline void SetStatus(const StatusType& value) { m_statusHasBeenSet = true; m_status = value; }
    inline void SetStatus(StatusType&& value) { m_statusHasBeenSet = true; m_status = std::move(value); }
    inline UpdateAccessKeyRequest& WithStatus(const StatusType& value) { SetStatus(value); return *this;}
    inline UpdateAccessKeyRequest& WithStatus(StatusType&& value) { SetStatus(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    Aws::String m_accessKeyId;
    bool m_accessKeyIdHasBeenSet = false;

    StatusType m_status;
    bool m_statusHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
