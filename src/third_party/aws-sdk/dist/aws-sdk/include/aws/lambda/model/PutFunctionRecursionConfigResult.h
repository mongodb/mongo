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
  class PutFunctionRecursionConfigResult
  {
  public:
    AWS_LAMBDA_API PutFunctionRecursionConfigResult();
    AWS_LAMBDA_API PutFunctionRecursionConfigResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API PutFunctionRecursionConfigResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The status of your function's recursive loop detection configuration.</p>
     * <p>When this value is set to <code>Allow</code>and Lambda detects your function
     * being invoked as part of a recursive loop, it doesn't take any action.</p>
     * <p>When this value is set to <code>Terminate</code> and Lambda detects your
     * function being invoked as part of a recursive loop, it stops your function being
     * invoked and notifies you. </p>
     */
    inline const RecursiveLoop& GetRecursiveLoop() const{ return m_recursiveLoop; }
    inline void SetRecursiveLoop(const RecursiveLoop& value) { m_recursiveLoop = value; }
    inline void SetRecursiveLoop(RecursiveLoop&& value) { m_recursiveLoop = std::move(value); }
    inline PutFunctionRecursionConfigResult& WithRecursiveLoop(const RecursiveLoop& value) { SetRecursiveLoop(value); return *this;}
    inline PutFunctionRecursionConfigResult& WithRecursiveLoop(RecursiveLoop&& value) { SetRecursiveLoop(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline PutFunctionRecursionConfigResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline PutFunctionRecursionConfigResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline PutFunctionRecursionConfigResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    RecursiveLoop m_recursiveLoop;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
