/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/IAMRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/Tag.h>
#include <utility>

namespace Aws
{
namespace IAM
{
namespace Model
{

  /**
   */
  class UploadServerCertificateRequest : public IAMRequest
  {
  public:
    AWS_IAM_API UploadServerCertificateRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UploadServerCertificate"; }

    AWS_IAM_API Aws::String SerializePayload() const override;

  protected:
    AWS_IAM_API void DumpBodyToUrl(Aws::Http::URI& uri ) const override;

  public:

    ///@{
    /**
     * <p>The path for the server certificate. For more information about paths, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/Using_Identifiers.html">IAM
     * identifiers</a> in the <i>IAM User Guide</i>.</p> <p>This parameter is optional.
     * If it is not included, it defaults to a slash (/). This parameter allows
     * (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>) a
     * string of characters consisting of either a forward slash (/) by itself or a
     * string that must begin and end with forward slashes. In addition, it can contain
     * any ASCII character from the ! (<code>\u0021</code>) through the DEL character
     * (<code>\u007F</code>), including most punctuation characters, digits, and upper
     * and lowercased letters.</p>  <p> If you are uploading a server certificate
     * specifically for use with Amazon CloudFront distributions, you must specify a
     * path using the <code>path</code> parameter. The path must begin with
     * <code>/cloudfront</code> and must include a trailing slash (for example,
     * <code>/cloudfront/test/</code>).</p> 
     */
    inline const Aws::String& GetPath() const{ return m_path; }
    inline bool PathHasBeenSet() const { return m_pathHasBeenSet; }
    inline void SetPath(const Aws::String& value) { m_pathHasBeenSet = true; m_path = value; }
    inline void SetPath(Aws::String&& value) { m_pathHasBeenSet = true; m_path = std::move(value); }
    inline void SetPath(const char* value) { m_pathHasBeenSet = true; m_path.assign(value); }
    inline UploadServerCertificateRequest& WithPath(const Aws::String& value) { SetPath(value); return *this;}
    inline UploadServerCertificateRequest& WithPath(Aws::String&& value) { SetPath(std::move(value)); return *this;}
    inline UploadServerCertificateRequest& WithPath(const char* value) { SetPath(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name for the server certificate. Do not include the path in this value.
     * The name of the certificate cannot contain any spaces.</p> <p>This parameter
     * allows (through its <a href="http://wikipedia.org/wiki/regex">regex pattern</a>)
     * a string of characters consisting of upper and lowercase alphanumeric characters
     * with no spaces. You can also include any of the following characters:
     * _+=,.@-</p>
     */
    inline const Aws::String& GetServerCertificateName() const{ return m_serverCertificateName; }
    inline bool ServerCertificateNameHasBeenSet() const { return m_serverCertificateNameHasBeenSet; }
    inline void SetServerCertificateName(const Aws::String& value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName = value; }
    inline void SetServerCertificateName(Aws::String&& value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName = std::move(value); }
    inline void SetServerCertificateName(const char* value) { m_serverCertificateNameHasBeenSet = true; m_serverCertificateName.assign(value); }
    inline UploadServerCertificateRequest& WithServerCertificateName(const Aws::String& value) { SetServerCertificateName(value); return *this;}
    inline UploadServerCertificateRequest& WithServerCertificateName(Aws::String&& value) { SetServerCertificateName(std::move(value)); return *this;}
    inline UploadServerCertificateRequest& WithServerCertificateName(const char* value) { SetServerCertificateName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The contents of the public key certificate in PEM-encoded format.</p> <p>The
     * <a href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate
     * this parameter is a string of characters consisting of the following:</p> <ul>
     * <li> <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul>
     */
    inline const Aws::String& GetCertificateBody() const{ return m_certificateBody; }
    inline bool CertificateBodyHasBeenSet() const { return m_certificateBodyHasBeenSet; }
    inline void SetCertificateBody(const Aws::String& value) { m_certificateBodyHasBeenSet = true; m_certificateBody = value; }
    inline void SetCertificateBody(Aws::String&& value) { m_certificateBodyHasBeenSet = true; m_certificateBody = std::move(value); }
    inline void SetCertificateBody(const char* value) { m_certificateBodyHasBeenSet = true; m_certificateBody.assign(value); }
    inline UploadServerCertificateRequest& WithCertificateBody(const Aws::String& value) { SetCertificateBody(value); return *this;}
    inline UploadServerCertificateRequest& WithCertificateBody(Aws::String&& value) { SetCertificateBody(std::move(value)); return *this;}
    inline UploadServerCertificateRequest& WithCertificateBody(const char* value) { SetCertificateBody(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The contents of the private key in PEM-encoded format.</p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate this
     * parameter is a string of characters consisting of the following:</p> <ul> <li>
     * <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul>
     */
    inline const Aws::String& GetPrivateKey() const{ return m_privateKey; }
    inline bool PrivateKeyHasBeenSet() const { return m_privateKeyHasBeenSet; }
    inline void SetPrivateKey(const Aws::String& value) { m_privateKeyHasBeenSet = true; m_privateKey = value; }
    inline void SetPrivateKey(Aws::String&& value) { m_privateKeyHasBeenSet = true; m_privateKey = std::move(value); }
    inline void SetPrivateKey(const char* value) { m_privateKeyHasBeenSet = true; m_privateKey.assign(value); }
    inline UploadServerCertificateRequest& WithPrivateKey(const Aws::String& value) { SetPrivateKey(value); return *this;}
    inline UploadServerCertificateRequest& WithPrivateKey(Aws::String&& value) { SetPrivateKey(std::move(value)); return *this;}
    inline UploadServerCertificateRequest& WithPrivateKey(const char* value) { SetPrivateKey(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The contents of the certificate chain. This is typically a concatenation of
     * the PEM-encoded public key certificates of the chain.</p> <p>The <a
     * href="http://wikipedia.org/wiki/regex">regex pattern</a> used to validate this
     * parameter is a string of characters consisting of the following:</p> <ul> <li>
     * <p>Any printable ASCII character ranging from the space character
     * (<code>\u0020</code>) through the end of the ASCII character range</p> </li>
     * <li> <p>The printable characters in the Basic Latin and Latin-1 Supplement
     * character set (through <code>\u00FF</code>)</p> </li> <li> <p>The special
     * characters tab (<code>\u0009</code>), line feed (<code>\u000A</code>), and
     * carriage return (<code>\u000D</code>)</p> </li> </ul>
     */
    inline const Aws::String& GetCertificateChain() const{ return m_certificateChain; }
    inline bool CertificateChainHasBeenSet() const { return m_certificateChainHasBeenSet; }
    inline void SetCertificateChain(const Aws::String& value) { m_certificateChainHasBeenSet = true; m_certificateChain = value; }
    inline void SetCertificateChain(Aws::String&& value) { m_certificateChainHasBeenSet = true; m_certificateChain = std::move(value); }
    inline void SetCertificateChain(const char* value) { m_certificateChainHasBeenSet = true; m_certificateChain.assign(value); }
    inline UploadServerCertificateRequest& WithCertificateChain(const Aws::String& value) { SetCertificateChain(value); return *this;}
    inline UploadServerCertificateRequest& WithCertificateChain(Aws::String&& value) { SetCertificateChain(std::move(value)); return *this;}
    inline UploadServerCertificateRequest& WithCertificateChain(const char* value) { SetCertificateChain(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that you want to attach to the new IAM server certificate
     * resource. Each tag consists of a key name and an associated value. For more
     * information about tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>  <p>If any one of the tags
     * is invalid or if you exceed the allowed maximum number of tags, then the entire
     * request fails and the resource is not created.</p> 
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline UploadServerCertificateRequest& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline UploadServerCertificateRequest& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline UploadServerCertificateRequest& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline UploadServerCertificateRequest& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}
  private:

    Aws::String m_path;
    bool m_pathHasBeenSet = false;

    Aws::String m_serverCertificateName;
    bool m_serverCertificateNameHasBeenSet = false;

    Aws::String m_certificateBody;
    bool m_certificateBodyHasBeenSet = false;

    Aws::String m_privateKey;
    bool m_privateKeyHasBeenSet = false;

    Aws::String m_certificateChain;
    bool m_certificateChainHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
