/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/iam/model/ServerCertificateMetadata.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/Tag.h>
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
   * <p>Contains information about a server certificate.</p> <p> This data type is
   * used as a response element in the <a>GetServerCertificate</a> operation.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/ServerCertificate">AWS
   * API Reference</a></p>
   */
  class ServerCertificate
  {
  public:
    AWS_IAM_API ServerCertificate();
    AWS_IAM_API ServerCertificate(const Aws::Utils::Xml::XmlNode& xmlNode);
    AWS_IAM_API ServerCertificate& operator=(const Aws::Utils::Xml::XmlNode& xmlNode);

    AWS_IAM_API void OutputToStream(Aws::OStream& ostream, const char* location, unsigned index, const char* locationValue) const;
    AWS_IAM_API void OutputToStream(Aws::OStream& oStream, const char* location) const;


    ///@{
    /**
     * <p>The meta information of the server certificate, such as its name, path, ID,
     * and ARN.</p>
     */
    inline const ServerCertificateMetadata& GetServerCertificateMetadata() const{ return m_serverCertificateMetadata; }
    inline bool ServerCertificateMetadataHasBeenSet() const { return m_serverCertificateMetadataHasBeenSet; }
    inline void SetServerCertificateMetadata(const ServerCertificateMetadata& value) { m_serverCertificateMetadataHasBeenSet = true; m_serverCertificateMetadata = value; }
    inline void SetServerCertificateMetadata(ServerCertificateMetadata&& value) { m_serverCertificateMetadataHasBeenSet = true; m_serverCertificateMetadata = std::move(value); }
    inline ServerCertificate& WithServerCertificateMetadata(const ServerCertificateMetadata& value) { SetServerCertificateMetadata(value); return *this;}
    inline ServerCertificate& WithServerCertificateMetadata(ServerCertificateMetadata&& value) { SetServerCertificateMetadata(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The contents of the public key certificate.</p>
     */
    inline const Aws::String& GetCertificateBody() const{ return m_certificateBody; }
    inline bool CertificateBodyHasBeenSet() const { return m_certificateBodyHasBeenSet; }
    inline void SetCertificateBody(const Aws::String& value) { m_certificateBodyHasBeenSet = true; m_certificateBody = value; }
    inline void SetCertificateBody(Aws::String&& value) { m_certificateBodyHasBeenSet = true; m_certificateBody = std::move(value); }
    inline void SetCertificateBody(const char* value) { m_certificateBodyHasBeenSet = true; m_certificateBody.assign(value); }
    inline ServerCertificate& WithCertificateBody(const Aws::String& value) { SetCertificateBody(value); return *this;}
    inline ServerCertificate& WithCertificateBody(Aws::String&& value) { SetCertificateBody(std::move(value)); return *this;}
    inline ServerCertificate& WithCertificateBody(const char* value) { SetCertificateBody(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The contents of the public key certificate chain.</p>
     */
    inline const Aws::String& GetCertificateChain() const{ return m_certificateChain; }
    inline bool CertificateChainHasBeenSet() const { return m_certificateChainHasBeenSet; }
    inline void SetCertificateChain(const Aws::String& value) { m_certificateChainHasBeenSet = true; m_certificateChain = value; }
    inline void SetCertificateChain(Aws::String&& value) { m_certificateChainHasBeenSet = true; m_certificateChain = std::move(value); }
    inline void SetCertificateChain(const char* value) { m_certificateChainHasBeenSet = true; m_certificateChain.assign(value); }
    inline ServerCertificate& WithCertificateChain(const Aws::String& value) { SetCertificateChain(value); return *this;}
    inline ServerCertificate& WithCertificateChain(Aws::String&& value) { SetCertificateChain(std::move(value)); return *this;}
    inline ServerCertificate& WithCertificateChain(const char* value) { SetCertificateChain(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that are attached to the server certificate. For more
     * information about tagging, see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline bool TagsHasBeenSet() const { return m_tagsHasBeenSet; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tagsHasBeenSet = true; m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tagsHasBeenSet = true; m_tags = std::move(value); }
    inline ServerCertificate& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline ServerCertificate& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline ServerCertificate& AddTags(const Tag& value) { m_tagsHasBeenSet = true; m_tags.push_back(value); return *this; }
    inline ServerCertificate& AddTags(Tag&& value) { m_tagsHasBeenSet = true; m_tags.push_back(std::move(value)); return *this; }
    ///@}
  private:

    ServerCertificateMetadata m_serverCertificateMetadata;
    bool m_serverCertificateMetadataHasBeenSet = false;

    Aws::String m_certificateBody;
    bool m_certificateBodyHasBeenSet = false;

    Aws::String m_certificateChain;
    bool m_certificateChainHasBeenSet = false;

    Aws::Vector<Tag> m_tags;
    bool m_tagsHasBeenSet = false;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
