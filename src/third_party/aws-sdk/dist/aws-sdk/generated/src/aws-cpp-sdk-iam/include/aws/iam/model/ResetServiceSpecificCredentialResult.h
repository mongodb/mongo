/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/iam/model/ServiceSpecificCredential.h>
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
  class ResetServiceSpecificCredentialResult
  {
  public:
    AWS_IAM_API ResetServiceSpecificCredentialResult();
    AWS_IAM_API ResetServiceSpecificCredentialResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API ResetServiceSpecificCredentialResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A structure with details about the updated service-specific credential,
     * including the new password.</p>  <p>This is the <b>only</b> time that
     * you can access the password. You cannot recover the password later, but you can
     * reset it again.</p> 
     */
    inline const ServiceSpecificCredential& GetServiceSpecificCredential() const{ return m_serviceSpecificCredential; }
    inline void SetServiceSpecificCredential(const ServiceSpecificCredential& value) { m_serviceSpecificCredential = value; }
    inline void SetServiceSpecificCredential(ServiceSpecificCredential&& value) { m_serviceSpecificCredential = std::move(value); }
    inline ResetServiceSpecificCredentialResult& WithServiceSpecificCredential(const ServiceSpecificCredential& value) { SetServiceSpecificCredential(value); return *this;}
    inline ResetServiceSpecificCredentialResult& WithServiceSpecificCredential(ServiceSpecificCredential&& value) { SetServiceSpecificCredential(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline ResetServiceSpecificCredentialResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline ResetServiceSpecificCredentialResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    ServiceSpecificCredential m_serviceSpecificCredential;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
