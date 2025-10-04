/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/lambda/model/FunctionConfiguration.h>
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
  /**
   * <p>A list of Lambda functions.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/ListFunctionsResponse">AWS
   * API Reference</a></p>
   */
  class ListFunctionsResult
  {
  public:
    AWS_LAMBDA_API ListFunctionsResult();
    AWS_LAMBDA_API ListFunctionsResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API ListFunctionsResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The pagination token that's included if more results are available.</p>
     */
    inline const Aws::String& GetNextMarker() const{ return m_nextMarker; }
    inline void SetNextMarker(const Aws::String& value) { m_nextMarker = value; }
    inline void SetNextMarker(Aws::String&& value) { m_nextMarker = std::move(value); }
    inline void SetNextMarker(const char* value) { m_nextMarker.assign(value); }
    inline ListFunctionsResult& WithNextMarker(const Aws::String& value) { SetNextMarker(value); return *this;}
    inline ListFunctionsResult& WithNextMarker(Aws::String&& value) { SetNextMarker(std::move(value)); return *this;}
    inline ListFunctionsResult& WithNextMarker(const char* value) { SetNextMarker(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A list of Lambda functions.</p>
     */
    inline const Aws::Vector<FunctionConfiguration>& GetFunctions() const{ return m_functions; }
    inline void SetFunctions(const Aws::Vector<FunctionConfiguration>& value) { m_functions = value; }
    inline void SetFunctions(Aws::Vector<FunctionConfiguration>&& value) { m_functions = std::move(value); }
    inline ListFunctionsResult& WithFunctions(const Aws::Vector<FunctionConfiguration>& value) { SetFunctions(value); return *this;}
    inline ListFunctionsResult& WithFunctions(Aws::Vector<FunctionConfiguration>&& value) { SetFunctions(std::move(value)); return *this;}
    inline ListFunctionsResult& AddFunctions(const FunctionConfiguration& value) { m_functions.push_back(value); return *this; }
    inline ListFunctionsResult& AddFunctions(FunctionConfiguration&& value) { m_functions.push_back(std::move(value)); return *this; }
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline ListFunctionsResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline ListFunctionsResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline ListFunctionsResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_nextMarker;

    Aws::Vector<FunctionConfiguration> m_functions;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
