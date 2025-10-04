/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/CodeSigningConfig.h>
#include <aws/core/utils/memory/stl/AWSString.h>
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
  class CreateCodeSigningConfigResult
  {
  public:
    AWS_LAMBDA_API CreateCodeSigningConfigResult();
    AWS_LAMBDA_API CreateCodeSigningConfigResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API CreateCodeSigningConfigResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The code signing configuration.</p>
     */
    inline const CodeSigningConfig& GetCodeSigningConfig() const{ return m_codeSigningConfig; }
    inline void SetCodeSigningConfig(const CodeSigningConfig& value) { m_codeSigningConfig = value; }
    inline void SetCodeSigningConfig(CodeSigningConfig&& value) { m_codeSigningConfig = std::move(value); }
    inline CreateCodeSigningConfigResult& WithCodeSigningConfig(const CodeSigningConfig& value) { SetCodeSigningConfig(value); return *this;}
    inline CreateCodeSigningConfigResult& WithCodeSigningConfig(CodeSigningConfig&& value) { SetCodeSigningConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline CreateCodeSigningConfigResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline CreateCodeSigningConfigResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline CreateCodeSigningConfigResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    CodeSigningConfig m_codeSigningConfig;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
