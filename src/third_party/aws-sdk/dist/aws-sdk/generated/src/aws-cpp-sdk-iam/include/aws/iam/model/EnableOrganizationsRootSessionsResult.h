/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/iam/model/ResponseMetadata.h>
#include <aws/iam/model/FeatureType.h>
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
  class EnableOrganizationsRootSessionsResult
  {
  public:
    AWS_IAM_API EnableOrganizationsRootSessionsResult();
    AWS_IAM_API EnableOrganizationsRootSessionsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API EnableOrganizationsRootSessionsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The unique identifier (ID) of an organization.</p>
     */
    inline const Aws::String& GetOrganizationId() const{ return m_organizationId; }
    inline void SetOrganizationId(const Aws::String& value) { m_organizationId = value; }
    inline void SetOrganizationId(Aws::String&& value) { m_organizationId = std::move(value); }
    inline void SetOrganizationId(const char* value) { m_organizationId.assign(value); }
    inline EnableOrganizationsRootSessionsResult& WithOrganizationId(const Aws::String& value) { SetOrganizationId(value); return *this;}
    inline EnableOrganizationsRootSessionsResult& WithOrganizationId(Aws::String&& value) { SetOrganizationId(std::move(value)); return *this;}
    inline EnableOrganizationsRootSessionsResult& WithOrganizationId(const char* value) { SetOrganizationId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The features you have enabled for centralized root access.</p>
     */
    inline const Aws::Vector<FeatureType>& GetEnabledFeatures() const{ return m_enabledFeatures; }
    inline void SetEnabledFeatures(const Aws::Vector<FeatureType>& value) { m_enabledFeatures = value; }
    inline void SetEnabledFeatures(Aws::Vector<FeatureType>&& value) { m_enabledFeatures = std::move(value); }
    inline EnableOrganizationsRootSessionsResult& WithEnabledFeatures(const Aws::Vector<FeatureType>& value) { SetEnabledFeatures(value); return *this;}
    inline EnableOrganizationsRootSessionsResult& WithEnabledFeatures(Aws::Vector<FeatureType>&& value) { SetEnabledFeatures(std::move(value)); return *this;}
    inline EnableOrganizationsRootSessionsResult& AddEnabledFeatures(const FeatureType& value) { m_enabledFeatures.push_back(value); return *this; }
    inline EnableOrganizationsRootSessionsResult& AddEnabledFeatures(FeatureType&& value) { m_enabledFeatures.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline EnableOrganizationsRootSessionsResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline EnableOrganizationsRootSessionsResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_organizationId;

    Aws::Vector<FeatureType> m_enabledFeatures;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
