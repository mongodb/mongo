/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/model/ServerCertificateMetadata.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/ResponseMetadata.h>
#include <aws/iam/model/Tag.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Xml
{
  class XmlDocument;
} // namespace Xml
} // namespace Utils
namespace IAM
{
namespace Model
{
  /**
   * <p>Contains the response to a successful <a>UploadServerCertificate</a> request.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/UploadServerCertificateResponse">AWS
   * API Reference</a></p>
   */
  class UploadServerCertificateResult
  {
  public:
    AWS_IAM_API UploadServerCertificateResult();
    AWS_IAM_API UploadServerCertificateResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API UploadServerCertificateResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The meta information of the uploaded server certificate without its
     * certificate body, certificate chain, and private key.</p>
     */
    inline const ServerCertificateMetadata& GetServerCertificateMetadata() const{ return m_serverCertificateMetadata; }
    inline void SetServerCertificateMetadata(const ServerCertificateMetadata& value) { m_serverCertificateMetadata = value; }
    inline void SetServerCertificateMetadata(ServerCertificateMetadata&& value) { m_serverCertificateMetadata = std::move(value); }
    inline UploadServerCertificateResult& WithServerCertificateMetadata(const ServerCertificateMetadata& value) { SetServerCertificateMetadata(value); return *this;}
    inline UploadServerCertificateResult& WithServerCertificateMetadata(ServerCertificateMetadata&& value) { SetServerCertificateMetadata(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of tags that are attached to the new IAM server certificate. The
     * returned list of tags is sorted by tag key. For more information about tagging,
     * see <a
     * href="https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html">Tagging IAM
     * resources</a> in the <i>IAM User Guide</i>.</p>
     */
    inline const Aws::Vector<Tag>& GetTags() const{ return m_tags; }
    inline void SetTags(const Aws::Vector<Tag>& value) { m_tags = value; }
    inline void SetTags(Aws::Vector<Tag>&& value) { m_tags = std::move(value); }
    inline UploadServerCertificateResult& WithTags(const Aws::Vector<Tag>& value) { SetTags(value); return *this;}
    inline UploadServerCertificateResult& WithTags(Aws::Vector<Tag>&& value) { SetTags(std::move(value)); return *this;}
    inline UploadServerCertificateResult& AddTags(const Tag& value) { m_tags.push_back(value); return *this; }
    inline UploadServerCertificateResult& AddTags(Tag&& value) { m_tags.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline UploadServerCertificateResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline UploadServerCertificateResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    ServerCertificateMetadata m_serverCertificateMetadata;

    Aws::Vector<Tag> m_tags;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
