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
  class PutFunctionConcurrencyResult
  {
  public:
    AWS_LAMBDA_API PutFunctionConcurrencyResult();
    AWS_LAMBDA_API PutFunctionConcurrencyResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API PutFunctionConcurrencyResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The number of concurrent executions that are reserved for this function. For
     * more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/configuration-concurrency.html">Managing
     * Lambda reserved concurrency</a>.</p>
     */
    inline int GetReservedConcurrentExecutions() const{ return m_reservedConcurrentExecutions; }
    inline void SetReservedConcurrentExecutions(int value) { m_reservedConcurrentExecutions = value; }
    inline PutFunctionConcurrencyResult& WithReservedConcurrentExecutions(int value) { SetReservedConcurrentExecutions(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline PutFunctionConcurrencyResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline PutFunctionConcurrencyResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline PutFunctionConcurrencyResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    int m_reservedConcurrentExecutions;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
