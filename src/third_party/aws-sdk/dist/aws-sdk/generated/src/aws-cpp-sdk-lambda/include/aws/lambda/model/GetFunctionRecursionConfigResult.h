/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/model/RecursiveLoop.h>
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
  class GetFunctionRecursionConfigResult
  {
  public:
    AWS_LAMBDA_API GetFunctionRecursionConfigResult();
    AWS_LAMBDA_API GetFunctionRecursionConfigResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API GetFunctionRecursionConfigResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>If your function's recursive loop detection configuration is
     * <code>Allow</code>, Lambda doesn't take any action when it detects your function
     * being invoked as part of a recursive loop.</p> <p>If your function's recursive
     * loop detection configuration is <code>Terminate</code>, Lambda stops your
     * function being invoked and notifies you when it detects your function being
     * invoked as part of a recursive loop.</p> <p>By default, Lambda sets your
     * function's configuration to <code>Terminate</code>. You can update this
     * configuration using the <a>PutFunctionRecursionConfig</a> action.</p>
     */
    inline const RecursiveLoop& GetRecursiveLoop() const{ return m_recursiveLoop; }
    inline void SetRecursiveLoop(const RecursiveLoop& value) { m_recursiveLoop = value; }
    inline void SetRecursiveLoop(RecursiveLoop&& value) { m_recursiveLoop = std::move(value); }
    inline GetFunctionRecursionConfigResult& WithRecursiveLoop(const RecursiveLoop& value) { SetRecursiveLoop(value); return *this;}
    inline GetFunctionRecursionConfigResult& WithRecursiveLoop(RecursiveLoop&& value) { SetRecursiveLoop(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetFunctionRecursionConfigResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetFunctionRecursionConfigResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetFunctionRecursionConfigResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    RecursiveLoop m_recursiveLoop;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
