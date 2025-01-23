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
  class DeleteSigningCertificateRequest : public IAMRequest
  {
  public:
    AWS_IAM_API DeleteSigningCertificateRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "DeleteSigningCertificate"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the user the signing certificate belongs to.</p> <p>This
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
    inline DeleteSigningCertificateRequest& WithUserName(const Aws::String& value) { SetUserName(value); return *this;}
    inline DeleteSigningCertificateRequest& WithUserName(Aws::String&& value) { SetUserName(std::move(value)); return *this;}
    inline DeleteSigningCertificateRequest& WithUserName(const char* value) { SetUserName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The ID of the signing certificate to delete.</p> <p>The format of this
     * parameter, as described by its <a
     * href="http://wikipedia.org/wiki/regex">regex</a> pattern, is a string of
     * characters that can be upper- or lower-cased letters or digits.</p>
     */
    inline const Aws::String& GetCertificateId() const{ return m_certificateId; }
    inline bool CertificateIdHasBeenSet() const { return m_certificateIdHasBeenSet; }
    inline void SetCertificateId(const Aws::String& value) { m_certificateIdHasBeenSet = true; m_certificateId = value; }
    inline void SetCertificateId(Aws::String&& value) { m_certificateIdHasBeenSet = true; m_certificateId = std::move(value); }
    inline void SetCertificateId(const char* value) { m_certificateIdHasBeenSet = true; m_certificateId.assign(value); }
    inline DeleteSigningCertificateRequest& WithCertificateId(const Aws::String& value) { SetCertificateId(value); return *this;}
    inline DeleteSigningCertificateRequest& WithCertificateId(Aws::String&& value) { SetCertificateId(std::move(value)); return *this;}
    inline DeleteSigningCertificateRequest& WithCertificateId(const char* value) { SetCertificateId(value); return *this;}
    ///@}
  private:

    Aws::String m_userName;
    bool m_userNameHasBeenSet = false;

    Aws::String m_certificateId;
    bool m_certificateIdHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
