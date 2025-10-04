/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/model/ServerCertificate.h>
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
   * <p>Contains the response to a successful <a>GetServerCertificate</a> request.
   * </p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/iam-2010-05-08/GetServerCertificateResponse">AWS
   * API Reference</a></p>
   */
  class GetServerCertificateResult
  {
  public:
    AWS_IAM_API GetServerCertificateResult();
    AWS_IAM_API GetServerCertificateResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API GetServerCertificateResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A structure containing details about the server certificate.</p>
     */
    inline const ServerCertificate& GetServerCertificate() const{ return m_serverCertificate; }
    inline void SetServerCertificate(const ServerCertificate& value) { m_serverCertificate = value; }
    inline void SetServerCertificate(ServerCertificate&& value) { m_serverCertificate = std::move(value); }
    inline GetServerCertificateResult& WithServerCertificate(const ServerCertificate& value) { SetServerCertificate(value); return *this;}
    inline GetServerCertificateResult& WithServerCertificate(ServerCertificate&& value) { SetServerCertificate(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline GetServerCertificateResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline GetServerCertificateResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    ServerCertificate m_serverCertificate;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
