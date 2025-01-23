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
  class GetFunctionConcurrencyResult
  {
  public:
    AWS_LAMBDA_API GetFunctionConcurrencyResult();
    AWS_LAMBDA_API GetFunctionConcurrencyResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API GetFunctionConcurrencyResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The number of simultaneous executions that are reserved for the function.</p>
     */
    inline int GetReservedConcurrentExecutions() const{ return m_reservedConcurrentExecutions; }
    inline void SetReservedConcurrentExecutions(int value) { m_reservedConcurrentExecutions = value; }
    inline GetFunctionConcurrencyResult& WithReservedConcurrentExecutions(int value) { SetReservedConcurrentExecutions(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline GetFunctionConcurrencyResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline GetFunctionConcurrencyResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline GetFunctionConcurrencyResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    int m_reservedConcurrentExecutions;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
