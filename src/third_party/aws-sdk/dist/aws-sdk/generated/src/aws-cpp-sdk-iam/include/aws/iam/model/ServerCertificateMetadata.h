/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/DateTime.h>
#include <utility>

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
   * <p>Contains information about a server certificate without its certificate body,
   * certificate chain, and private key.</p> <p> This data type is used as a response
   * element in the <a>UploadServerCertificate</a> and <a>ListServerCertificates</a>
   * operations. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/ServerCertificateMetadata">AWS
   * API Reference</a></p>
   */
  class ServerCertificateMetadata
  {
  public:
    AWS_IAM_API ServerCertificateMetadata();
    AWS_IAM_API ServerCertificateMetadata(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API ServerCertificateMetadata& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p> The path to the server certificate. For more information about paths, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>. </p>
     */
    inline const Aws::String& GetPath() const{ return m_path; }
    inline bool PathHasBeenSet() const { return m_pathHasBeenSet; }
    inline void SetPath(const Aws::String& value) { m_pathHasBeenSet = true; m_path = value; }
    inline void SetPath(Aws::String&& value) { m_pathHasBeenSet = true; m_path = std::move(value); }
    inline void SetPath(const char* value) { m_pathHasBeenSet = true; m_path.assign(value); }
    inline ServerCertificateMetadata& WithPath(const Aws::String& value) { SetPath(value); return *this;}
    inline ServerCertificateMetadata& WithPath(Aws::String&& value) { SetPath(std::move(value)); return *this;}
    inline ServerCertificateMetadata& WithPath(const char* value) { SetPath(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name that identifies the server certificate.</p>
     */
    inline const Aws::String& GetServerCertificateName() const{ return m_serverCertificateName; }
    inline bool ServerCertificateNameHasBeenSet() const { return m_serverCertificateNameHasBeenSet; }
    inline void SetServerCertificateName(const Aws::String& value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName = value; }
    inline void SetServerCertificateName(Aws::String&& value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName = std::move(value); }
    inline void SetServerCertificateName(const char* value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName.assign(value); }
    inline ServerCertificateMetadata& WithServerCertificateName(const Aws::String& value) { SetServerCertificateName(value); return *this;}
    inline ServerCertificateMetadata& WithServerCertificateName(Aws::String&& value) { SetServerCertificateName(std::move(value)); return *this;}
    inline ServerCertificateMetadata& WithServerCertificateName(const char* value) { SetServerCertificateName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> The stable and unique string identifying the server certificate. For more
     * information about IDs, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>. </p>
     */
    inline const Aws::String& GetServerCertificateId() const{ return m_serverCertificateId; }
    inline bool ServerCertificateIdHasBeenSet() const { return m_serverCertificateIdHasBeenSet; }
    inline void SetServerCertificateId(const Aws::String& value) { m_serverCertificateIdHasBeenSet = true; m_serverCertificateId = value; }
    inline void SetServerCertificateId(Aws::String&& value) { m_serverCertificateIdHasBeenSet = true; m_serverCertificateId = std::move(value); }
    inline void SetServerCertificateId(const char* value) { m_serverCertificateIdHasBeenSet = true; m_serverCertificateId.assign(value); }
    inline ServerCertificateMetadata& WithServerCertificateId(const Aws::String& value) { SetServerCertificateId(value); return *this;}
    inline ServerCertificateMetadata& WithServerCertificateId(Aws::String&& value) { SetServerCertificateId(std::move(value)); return *this;}
    inline ServerCertificateMetadata& WithServerCertificateId(const char* value) { SetServerCertificateId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p> The Amazon Resource Name (ARN) specifying the server certificate. For more
     * information about ARNs and how to use them in policies, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>. </p>
     */
    inline const Aws::String& GetArn() const{ return m_arn; }
    inline bool ArnHasBeenSet() const { return m_arnHasBeenSet; }
    inline void SetArn(const Aws::String& value) { m_arnHasBeenSet = true; m_arn = value; }
    inline void SetArn(Aws::String&& value) { m_arnHasBeenSet = true; m_arn = std::move(value); }
    inline void SetArn(const char* value) { m_arnHasBeenSet = true; m_arn.assign(value); }
    inline ServerCertificateMetadata& WithArn(const Aws::String& value) { SetArn(value); return *this;}
    inline ServerCertificateMetadata& WithArn(Aws::String&& value) { SetArn(std::move(value)); return *this;}
    inline ServerCertificateMetadata& WithArn(const char* value) { SetArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date when the server certificate was uploaded.</p>
     */
    inline const Aws::Utils::DateTime& GetUploadDate() const{ return m_uploadDate; }
    inline bool UploadDateHasBeenSet() const { return m_uploadDateHasBeenSet; }
    inline void SetUploadDate(const Aws::Utils::DateTime& value) { m_uploadDateHasBeenSet = true; m_uploadDate = value; }
    inline void SetUploadDate(Aws::Utils::DateTime&& value) { m_uploadDateHasBeenSet = true; m_uploadDate = std::move(value); }
    inline ServerCertificateMetadata& WithUploadDate(const Aws::Utils::DateTime& value) { SetUploadDate(value); return *this;}
    inline ServerCertificateMetadata& WithUploadDate(Aws::Utils::DateTime&& value) { SetUploadDate(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The date on which the certificate is set to expire.</p>
     */
    inline const Aws::Utils::DateTime& GetExpiration() const{ return m_expiration; }
    inline bool ExpirationHasBeenSet() const { return m_expirationHasBeenSet; }
    inline void SetExpiration(const Aws::Utils::DateTime& value) { m_expirationHasBeenSet = true; m_expiration = value; }
    inline void SetExpiration(Aws::Utils::DateTime&& value) { m_expirationHasBeenSet = true; m_expiration = std::move(value); }
    inline ServerCertificateMetadata& WithExpiration(const Aws::Utils::DateTime& value) { SetExpiration(value); return *this;}
    inline ServerCertificateMetadata& WithExpiration(Aws::Utils::DateTime&& value) { SetExpiration(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_path;
    bool m_pathHasBeenSet = false;

    Aws::String m_serverCertificateName;
    bool m_serverCertificateNameHasBeenSet = false;

    Aws::String m_serverCertificateId;
    bool m_serverCertificateIdHasBeenSet = false;

    Aws::String m_arn;
    bool m_arnHasBeenSet = false;

    Aws::Utils::DateTime m_uploadDate;
    bool m_uploadDateHasBeenSet = false;

    Aws::Utils::DateTime m_expiration;
    bool m_expirationHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
