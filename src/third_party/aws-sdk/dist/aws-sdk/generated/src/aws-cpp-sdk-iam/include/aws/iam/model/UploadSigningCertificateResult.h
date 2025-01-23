/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/model/SigningCertificate.h>
#include <aws/iam/model/ResponseMetadata.h>
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
   * <p>Contains the response to a successful <a>UploadSigningCertificate</a>
   * request. </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/UploadSigningCertificateResponse">AWS
   * API Reference</a></p>
   */
  class UploadSigningCertificateResult
  {
  public:
    AWS_IAM_API UploadSigningCertificateResult();
    AWS_IAM_API UploadSigningCertificateResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API UploadSigningCertificateResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Information about the certificate.</p>
     */
    inline const SigningCertificate& GetCertificate() const{ return m_certificate; }
    inline void SetCertificate(const SigningCertificate& value) { m_certificate = value; }
    inline void SetCertificate(SigningCertificate&& value) { m_certificate = std::move(value); }
    inline UploadSigningCertificateResult& WithCertificate(const SigningCertificate& value) { SetCertificate(value); return *this;}
    inline UploadSigningCertificateResult& WithCertificate(SigningCertificate&& value) { SetCertificate(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline UploadSigningCertificateResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline UploadSigningCertificateResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    SigningCertificate m_certificate;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
