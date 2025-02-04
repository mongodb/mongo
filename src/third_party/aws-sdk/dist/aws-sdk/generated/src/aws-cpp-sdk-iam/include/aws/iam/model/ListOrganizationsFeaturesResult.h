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
  class ListOrganizationsFeaturesResult
  {
  public:
    AWS_IAM_API ListOrganizationsFeaturesResult();
    AWS_IAM_API ListOrganizationsFeaturesResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_IAM_API ListOrganizationsFeaturesResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>The unique identifier (ID) of an organization.</p>
     */
    inline const Aws::String& GetOrganizationId() const{ return m_organizationId; }
    inline void SetOrganizationId(const Aws::String& value) { m_organizationId = value; }
    inline void SetOrganizationId(Aws::String&& value) { m_organizationId = std::move(value); }
    inline void SetOrganizationId(const char* value) { m_organizationId.assign(value); }
    inline ListOrganizationsFeaturesResult& WithOrganizationId(const Aws::String& value) { SetOrganizationId(value); return *this;}
    inline ListOrganizationsFeaturesResult& WithOrganizationId(Aws::String&& value) { SetOrganizationId(std::move(value)); return *this;}
    inline ListOrganizationsFeaturesResult& WithOrganizationId(const char* value) { SetOrganizationId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Specifies the features that are currently available in your organization.</p>
     */
    inline const Aws::Vector<FeatureType>& GetEnabledFeatures() const{ return m_enabledFeatures; }
    inline void SetEnabledFeatures(const Aws::Vector<FeatureType>& value) { m_enabledFeatures = value; }
    inline void SetEnabledFeatures(Aws::Vector<FeatureType>&& value) { m_enabledFeatures = std::move(value); }
    inline ListOrganizationsFeaturesResult& WithEnabledFeatures(const Aws::Vector<FeatureType>& value) { SetEnabledFeatures(value); return *this;}
    inline ListOrganizationsFeaturesResult& WithEnabledFeatures(Aws::Vector<FeatureType>&& value) { SetEnabledFeatures(std::move(value)); return *this;}
    inline ListOrganizationsFeaturesResult& AddEnabledFeatures(const FeatureType& value) { m_enabledFeatures.push_back(value); return *this; }
    inline ListOrganizationsFeaturesResult& AddEnabledFeatures(FeatureType&& value) { m_enabledFeatures.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const ResponseMetadata& GetResponseMetadata() const{ return m_responseMetadata; }
    inline void SetResponseMetadata(const ResponseMetadata& value) { m_responseMetadata = value; }
    inline void SetResponseMetadata(ResponseMetadata&& value) { m_responseMetadata = std::move(value); }
    inline ListOrganizationsFeaturesResult& WithResponseMetadata(const ResponseMetadata& value) { SetResponseMetadata(value); return *this;}
    inline ListOrganizationsFeaturesResult& WithResponseMetadata(ResponseMetadata&& value) { SetResponseMetadata(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_organizationId;

    Aws::Vector<FeatureType> m_enabledFeatures;

    ResponseMetadata m_responseMetadata;
  };

} // namespace Model
} // namespace IAM
} // namespace Aws
