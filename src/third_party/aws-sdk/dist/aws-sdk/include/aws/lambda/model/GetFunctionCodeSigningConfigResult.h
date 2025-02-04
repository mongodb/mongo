/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
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
  class GetFunctionCodeSigningConfigResult
  {
  public:
    AWS_LAMBDA_API GetFunctionCodeSigningConfigResult();
    AWS_LAMBDA_API GetFunctionCodeSigningConfigResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API GetFunctionCodeSigningConfigResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The The Amazon Resource Name (ARN) of the code signing configuration.</p>
     */
    inline const Aws::String& GetCodeSigningConfigArn() const{ return m_codeSigningConfigArn; }
    inline void SetCodeSigningConfigArn(const Aws::String& value) { m_codeSigningConfigArn = value; }
    inline void SetCodeSigningConfigArn(Aws::String&& value) { m_codeSigningConfigArn = std::move(value); }
    inline void SetCodeSigningConfigArn(const char* value) { m_codeSigningConfigArn.assign(value); }
    inline GetFunctionCodeSigningConfigResult& WithCodeSigningConfigArn(const Aws::String& value) { SetCodeSigningConfigArn(value); return *this;}
    inline GetFunctionCodeSigningConfigResult& WithCodeSigningConfigArn(Aws::String&& value) { SetCodeSigningConfigArn(std::move(value)); return *this;}
    inline GetFunctionCodeSigningConfigResult& WithCodeSigningConfigArn(const char* value) { SetCodeSigningConfigArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The name or ARN of the Lambda function.</p> <p class="title"> <b>Name
     * formats</b> </p> <ul> <li> <p> <b>Function name</b> -
     * <code>MyFunction</code>.</p> </li> <li> <p> <b>Function ARN</b> -
     * <code>arn:aws:lambda:us-west-2:123456789012:function:MyFunction</code>.</p>
     * </li> <li> <p> <b>Partial ARN</b> -
     * <code>123456789012:function:MyFunction</code>.</p> </li> </ul> <p>The length
     * constraint applies only to the full ARN. If you specify only the function name,
     * it is limited to 64 characters in length.</p>
     */
    inline const Aws::String& GetFunctionName() const{ return m_functionName; }
    inline void SetFunctionName(const Aws::String& value) { m_functionName = value; }
    inline void SetFunctionName(Aws::String&& value) { m_functionName = std::move(value); }
    inline void SetFunctionName(const char* value) { m_functionName.assign(value); }
    inline GetFunctionCodeSigningConfigResult& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline GetFunctionCodeSigningConfigResult& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline GetFunctionCodeSigningConfigResult& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetFunctionCodeSigningConfigResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetFunctionCodeSigningConfigResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetFunctionCodeSigningConfigResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_codeSigningConfigArn;

    Aws::String m_functionName;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
