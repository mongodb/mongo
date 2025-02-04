/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/FunctionEventInvokeConfig.h>
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
  class ListFunctionEventInvokeConfigsResult
  {
  public:
    AWS_LAMBDA_API ListFunctionEventInvokeConfigsResult();
    AWS_LAMBDA_API ListFunctionEventInvokeConfigsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API ListFunctionEventInvokeConfigsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>A list of configurations.</p>
     */
    inline const Aws::Vector<FunctionEventInvokeConfig>& GetFunctionEventInvokeConfigs() const{ return m_functionEventInvokeConfigs; }
    inline void SetFunctionEventInvokeConfigs(const Aws::Vector<FunctionEventInvokeConfig>& value) { m_functionEventInvokeConfigs = value; }
    inline void SetFunctionEventInvokeConfigs(Aws::Vector<FunctionEventInvokeConfig>&& value) { m_functionEventInvokeConfigs = std::move(value); }
    inline ListFunctionEventInvokeConfigsResult& WithFunctionEventInvokeConfigs(const Aws::Vector<FunctionEventInvokeConfig>& value) { SetFunctionEventInvokeConfigs(value); return *this;}
    inline ListFunctionEventInvokeConfigsResult& WithFunctionEventInvokeConfigs(Aws::Vector<FunctionEventInvokeConfig>&& value) { SetFunctionEventInvokeConfigs(std::move(value)); return *this;}
    inline ListFunctionEventInvokeConfigsResult& AddFunctionEventInvokeConfigs(const FunctionEventInvokeConfig& value) { m_functionEventInvokeConfigs.push_back(value); return *this; }
    inline ListFunctionEventInvokeConfigsResult& AddFunctionEventInvokeConfigs(FunctionEventInvokeConfig&& value) { m_functionEventInvokeConfigs.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    /**
     * <p>The pagination token that's included if more results are available.</p>
     */
    inline const Aws::String& GetNextMarker() const{ return m_nextMarker; }
    inline void SetNextMarker(const Aws::String& value) { m_nextMarker = value; }
    inline void SetNextMarker(Aws::String&& value) { m_nextMarker = std::move(value); }
    inline void SetNextMarker(const char* value) { m_nextMarker.assign(value); }
    inline ListFunctionEventInvokeConfigsResult& WithNextMarker(const Aws::String& value) { SetNextMarker(value); return *this;}
    inline ListFunctionEventInvokeConfigsResult& WithNextMarker(Aws::String&& value) { SetNextMarker(std::move(value)); return *this;}
    inline ListFunctionEventInvokeConfigsResult& WithNextMarker(const char* value) { SetNextMarker(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListFunctionEventInvokeConfigsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListFunctionEventInvokeConfigsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListFunctionEventInvokeConfigsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Vector<FunctionEventInvokeConfig> m_functionEventInvokeConfigs;

    Aws::String m_nextMarker;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
