/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/model/IntelligentTieringConfiguration.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
namespace S3
{
namespace Model
{
  class GetBucketIntelligentTieringConfigurationResult
  {
  public:
    AWS_S3_API GetBucketIntelligentTieringConfigurationResult();
    AWS_S3_API GetBucketIntelligentTieringConfigurationResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetBucketIntelligentTieringConfigurationResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>Container for S3 Intelligent-Tiering configuration.</p>
     */
    inline const IntelligentTieringConfiguration& GetIntelligentTieringConfiguration() const{ return m_intelligentTieringConfiguration; }
    inline void SetIntelligentTieringConfiguration(const IntelligentTieringConfiguration& value) { m_intelligentTieringConfiguration = value; }
    inline void SetIntelligentTieringConfiguration(IntelligentTieringConfiguration&& value) { m_intelligentTieringConfiguration = std::move(value); }
    inline GetBucketIntelligentTieringConfigurationResult& WithIntelligentTieringConfiguration(const IntelligentTieringConfiguration& value) { SetIntelligentTieringConfiguration(value); return *this;}
    inline GetBucketIntelligentTieringConfigurationResult& WithIntelligentTieringConfiguration(IntelligentTieringConfiguration&& value) { SetIntelligentTieringConfiguration(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetBucketIntelligentTieringConfigurationResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetBucketIntelligentTieringConfigurationResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetBucketIntelligentTieringConfigurationResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    IntelligentTieringConfiguration m_intelligentTieringConfiguration;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
