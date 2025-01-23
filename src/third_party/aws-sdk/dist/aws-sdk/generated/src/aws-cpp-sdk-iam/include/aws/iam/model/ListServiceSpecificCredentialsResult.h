/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/ResponseMetadata.h>
#include <aws/iam/model/ServiceSpecificCredentialMetadata.h>
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
  class ListServiceSpecificCredentialsResult
  {
  public:
    AWS_IAM_API ListServiceSpecificCredentialsResult();
    AWS_IAM_API ListServiceSpecificCredentialsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API ListServiceSpecificCredentialsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A list of structures that each contain details about a service-specific
     * credential.</p>
     */
    inline const Aws::Vector<ServiceSpecificCredentialMetadata>& GetServiceSpecificCredentials() const{ return m_serviceSpecificCredentials; }
    inline void SetServiceSpecificCredentials(const Aws::Vector<ServiceSpecificCredentialMetadata>& value) { m_serviceSpecificCredentials = value; }
    inline void SetServiceSpecificCredentials(Aws::Vector<ServiceSpecificCredentialMetadata>&& value) { m_serviceSpecificCredentials = std::move(value); }
    inline ListServiceSpecificCredentialsResult& WithServiceSpecificCredentials(const Aws::Vector<ServiceSpecificCredentialMetadata>& value) { SetServiceSpecificCredentials(value); return *this;}
    inline ListServiceSpecificCredentialsResult& WithServiceSpecificCredentials(Aws::Vector<ServiceSpecificCredentialMetadata>&& value) { SetServiceSpecificCredentials(std::move(value)); return *this;}
    inline ListServiceSpecificCredentialsResult& AddServiceSpecificCredentials(const ServiceSpecificCredentialMetadata& value) { m_serviceSpecificCredentials.push_back(value); return *this; }
    inline ListServiceSpecificCredentialsResult& AddServiceSpecificCredentials(ServiceSpecificCredentialMetadata&& value) { m_serviceSpecificCredentials.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline ListServiceSpecificCredentialsResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline ListServiceSpecificCredentialsResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::Vector<ServiceSpecificCredentialMetadata> m_serviceSpecificCredentials;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
