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
  class UpdateServerCertificateRequest : public IAMRequest
  {
  public:
    AWS_IAM_API UpdateServerCertificateRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateServerCertificate"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The name of the server certificate that you want to update.</p> <p>This
     * parameter allows (through its <a href="http://wikipedia.org/wiki/regex">regex
     * pattern</a>) a string of characters consisting of upper and lowercase
     * alphanumeric characters with no spaces. You can also include any of the
     * following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetServerCertificateName() const{ return m_serverCertificateName; }
    inline bool ServerCertificateNameHasBeenSet() const { return m_serverCertificateNameHasBeenSet; }
    inline void SetServerCertificateName(const Aws::String& value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName = value; }
    inline void SetServerCertificateName(Aws::String&& value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName = std::move(value); }
    inline void SetServerCertificateName(const char* value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName.assign(value); }
    inline UpdateServerCertificateRequest& WithServerCertificateName(const Aws::String& value) { SetServerCertificateName(value); return *this;}
    inline UpdateServerCertificateRequest& WithServerCertificateName(Aws::String&& value) { SetServerCertificateName(std::move(value)); return *this;}
    inline UpdateServerCertificateRequest& WithServerCertificateName(const char* value) { SetServerCertificateName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The new path for the server certificate. Include this only if you are
     * updating the server certificate's path.</p> <p>This parameter allows (through
     * its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of
     * characters consisting of either a forward slash (/) by itself or a string that
     * must begin and end with forward slashes. In addition, it can contain any ASCII
     * character from the ! (<code>\u0021</code>) through the DEL character
     * (<code>\u007F</code>), including most punctuation characters, digits, and upper
     * and lowercased letters.</p>
     */
    inline const Aws::String& GetNewPath() const{ return m_newPath; }
    inline bool NewPathHasBeenSet() const { return m_newPathHasBeenSet; }
    inline void SetNewPath(const Aws::String& value) { m_newPathHasBeenSet = true; m_newPath = value; }
    inline void SetNewPath(Aws::String&& value) { m_newPathHasBeenSet = true; m_newPath = std::move(value); }
    inline void SetNewPath(const char* value) { m_newPathHasBeenSet = true; m_newPath.assign(value); }
    inline UpdateServerCertificateRequest& WithNewPath(const Aws::String& value) { SetNewPath(value); return *this;}
    inline UpdateServerCertificateRequest& WithNewPath(Aws::String&& value) { SetNewPath(std::move(value)); return *this;}
    inline UpdateServerCertificateRequest& WithNewPath(const char* value) { SetNewPath(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The new name for the server certificate. Include this only if you are
     * updating the server certificate's name. The name of the certificate cannot
     * contain any spaces.</p> <p>This parameter allows (through its <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a>) a string of characters
     * consisting of upper and lowercase alphanumeric characters with no spaces. You
     * can also include any of the following characters: _+=,.@-</p>
     */
    inline const Aws::String& GetNewServerCertificateName() const{ return m_newServerCertificateName; }
    inline bool NewServerCertificateNameHasBeenSet() const { return m_newServerCertificateNameHasBeenSet; }
    inline void SetNewServerCertificateName(const Aws::String& value) { m_newServerCertificateNameHasBeenSet = true; m_newServerCertificateName = value; }
    inline void SetNewServerCertificateName(Aws::String&& value) { m_newServerCertificateNameHasBeenSet = true; m_newServerCertificateName = std::move(value); }
    inline void SetNewServerCertificateName(const char* value) { m_newServerCertificateNameHasBeenSet = true; m_newServerCertificateName.assign(value); }
    inline UpdateServerCertificateRequest& WithNewServerCertificateName(const Aws::String& value) { SetNewServerCertificateName(value); return *this;}
    inline UpdateServerCertificateRequest& WithNewServerCertificateName(Aws::String&& value) { SetNewServerCertificateName(std::move(value)); return *this;}
    inline UpdateServerCertificateRequest& WithNewServerCertificateName(const char* value) { SetNewServerCertificateName(value); return *this;}
    ///@}
  private:

    Aws::String m_serverCertificateName;
    bool m_serverCertificateNameHasBeenSet = false;

    Aws::String m_newPath;
    bool m_newPathHasBeenSet = false;

    Aws::String m_newServerCertificateName;
    bool m_newServerCertificateNameHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
