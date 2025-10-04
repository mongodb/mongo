/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/s3/model/CORSRule.h>
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
  class GetBucketCorsResult
  {
  public:
    AWS_S3_API GetBucketCorsResult();
    AWS_S3_API GetBucketCorsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);
    AWS_S3_API GetBucketCorsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Xml::XmlDocument>& result);


    ///@{
    /**
     * <p>A set of origins and methods (cross-origin access that you want to allow).
     * You can add up to 100 rules to the configuration.</p>
     */
    inline const Aws::Vector<CORSRule>& GetCORSRules() const{ return m_cORSRules; }
    inline void SetCORSRules(const Aws::Vector<CORSRule>& value) { m_cORSRules = value; }
    inline void SetCORSRules(Aws::Vector<CORSRule>&& value) { m_cORSRules = std::move(value); }
    inline GetBucketCorsResult& WithCORSRules(const Aws::Vector<CORSRule>& value) { SetCORSRules(value); return *this;}
    inline GetBucketCorsResult& WithCORSRules(Aws::Vector<CORSRule>&& value) { SetCORSRules(std::move(value)); return *this;}
    inline GetBucketCorsResult& AddCORSRules(const CORSRule& value) { m_cORSRules.push_back(value); return *this; }
    inline GetBucketCorsResult& AddCORSRules(CORSRule&& value) { m_cORSRules.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetBucketCorsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetBucketCorsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetBucketCorsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<CORSRule> m_cORSRules;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace S3
} // namespace Aws
