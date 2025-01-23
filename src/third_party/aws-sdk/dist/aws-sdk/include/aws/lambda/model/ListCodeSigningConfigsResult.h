/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/CodeSigningConfig.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Utils
{
namespace Json
{
  class JsonValue;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{
  class ListCodeSigningConfigsResult
  {
  public:
    AWS_LAMBDA_API ListCodeSigningConfigsResult();
    AWS_LAMBDA_API ListCodeSigningConfigsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API ListCodeSigningConfigsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The pagination token that's included if more results are available.</p>
     */
    inline const Aws::String& GetNextMarker() const{ return m_nextMarker; }
    inline void SetNextMarker(const Aws::String& value) { m_nextMarker = value; }
    inline void SetNextMarker(Aws::String&& value) { m_nextMarker = std::move(value); }
    inline void SetNextMarker(const char* value) { m_nextMarker.assign(value); }
    inline ListCodeSigningConfigsResult& WithNextMarker(const Aws::String& value) { SetNextMarker(value); return *this;}
    inline ListCodeSigningConfigsResult& WithNextMarker(Aws::String&& value) { SetNextMarker(std::move(value)); return *this;}
    inline ListCodeSigningConfigsResult& WithNextMarker(const char* value) { SetNextMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The code signing configurations</p>
     */
    inline const Aws::Vector<CodeSigningConfig>& GetCodeSigningConfigs() const{ return m_codeSigningConfigs; }
    inline void SetCodeSigningConfigs(const Aws::Vector<CodeSigningConfig>& value) { m_codeSigningConfigs = value; }
    inline void SetCodeSigningConfigs(Aws::Vector<CodeSigningConfig>&& value) { m_codeSigningConfigs = std::move(value); }
    inline ListCodeSigningConfigsResult& WithCodeSigningConfigs(const Aws::Vector<CodeSigningConfig>& value) { SetCodeSigningConfigs(value); return *this;}
    inline ListCodeSigningConfigsResult& WithCodeSigningConfigs(Aws::Vector<CodeSigningConfig>&& value) { SetCodeSigningConfigs(std::move(value)); return *this;}
    inline ListCodeSigningConfigsResult& AddCodeSigningConfigs(const CodeSigningConfig& value) { m_codeSigningConfigs.push_back(value); return *this; }
    inline ListCodeSigningConfigsResult& AddCodeSigningConfigs(CodeSigningConfig&& value) { m_codeSigningConfigs.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListCodeSigningConfigsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListCodeSigningConfigsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListCodeSigningConfigsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_nextMarker;

    Aws::Vector<CodeSigningConfig> m_codeSigningConfigs;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
