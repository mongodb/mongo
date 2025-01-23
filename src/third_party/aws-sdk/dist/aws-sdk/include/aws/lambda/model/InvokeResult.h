/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/stream/ResponseStream.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/Array.h>
#include <utility>

namespace Aws
{
template<typename RESULT_TYPE>
class AmazonWebServiceResult;

namespace Lambda
{
namespace Model
{
  class InvokeResult
  {
  public:
    AWS_LAMBDA_API InvokeResult();
    //We have to define these because Microsoft doesn't auto generate them
    AWS_LAMBDA_API InvokeResult(InvokeResult&&);
    AWS_LAMBDA_API InvokeResult& operator=(InvokeResult&&);
    //we delete these because Microsoft doesn't handle move generation correctly
    //and we therefore don't trust them to get it right here either.
    InvokeResult(const InvokeResult&) = delete;
    InvokeResult& operator=(const InvokeResult&) = delete;


    AWS_LAMBDA_API InvokeResult(Aws::AmazonWebServiceResult<Aws::Utils::Stream::ResponseStream>&& result);
    AWS_LAMBDA_API InvokeResult& operator=(Aws::AmazonWebServiceResult<Aws::Utils::Stream::ResponseStream>&& result);



    ///@{
    /**
     * <p>The HTTP status code is in the 200 range for a successful request. For the
     * <code>RequestResponse</code> invocation type, this status code is 200. For the
     * <code>Event</code> invocation type, this status code is 202. For the
     * <code>DryRun</code> invocation type, the status code is 204.</p>
     */
    inline int GetStatusCode() const{ return m_statusCode; }
    inline void SetStatusCode(int value) { m_statusCode = value; }
    inline InvokeResult& WithStatusCode(int value) { SetStatusCode(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If present, indicates that an error occurred during function execution.
     * Details about the error are included in the response payload.</p>
     */
    inline const Aws::String& GetFunctionError() const{ return m_functionError; }
    inline void SetFunctionError(const Aws::String& value) { m_functionError = value; }
    inline void SetFunctionError(Aws::String&& value) { m_functionError = std::move(value); }
    inline void SetFunctionError(const char* value) { m_functionError.assign(value); }
    inline InvokeResult& WithFunctionError(const Aws::String& value) { SetFunctionError(value); return *this;}
    inline InvokeResult& WithFunctionError(Aws::String&& value) { SetFunctionError(std::move(value)); return *this;}
    inline InvokeResult& WithFunctionError(const char* value) { SetFunctionError(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The last 4 KB of the execution log, which is base64-encoded.</p>
     */
    inline const Aws::String& GetLogResult() const{ return m_logResult; }
    inline void SetLogResult(const Aws::String& value) { m_logResult = value; }
    inline void SetLogResult(Aws::String&& value) { m_logResult = std::move(value); }
    inline void SetLogResult(const char* value) { m_logResult.assign(value); }
    inline InvokeResult& WithLogResult(const Aws::String& value) { SetLogResult(value); return *this;}
    inline InvokeResult& WithLogResult(Aws::String&& value) { SetLogResult(std::move(value)); return *this;}
    inline InvokeResult& WithLogResult(const char* value) { SetLogResult(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The response from the function, or an error object.</p>
     */
    inline Aws::IOStream& GetPayload() const { return m_payload.GetUnderlyingStream(); }
    inline void ReplaceBody(Aws::IOStream* body) { m_payload = Aws::Utils::Stream::ResponseStream(body); }

    ///@}

    ///@{
    /**
     * <p>The version of the function that executed. When you invoke a function with an
     * alias, this indicates which version the alias resolved to.</p>
     */
    inline const Aws::String& GetExecutedVersion() const{ return m_executedVersion; }
    inline void SetExecutedVersion(const Aws::String& value) { m_executedVersion = value; }
    inline void SetExecutedVersion(Aws::String&& value) { m_executedVersion = std::move(value); }
    inline void SetExecutedVersion(const char* value) { m_executedVersion.assign(value); }
    inline InvokeResult& WithExecutedVersion(const Aws::String& value) { SetExecutedVersion(value); return *this;}
    inline InvokeResult& WithExecutedVersion(Aws::String&& value) { SetExecutedVersion(std::move(value)); return *this;}
    inline InvokeResult& WithExecutedVersion(const char* value) { SetExecutedVersion(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline InvokeResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline InvokeResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline InvokeResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    int m_statusCode;

    Aws::String m_functionError;

    Aws::String m_logResult;

    Aws::Utils::Stream::ResponseStream m_payload;

    Aws::String m_executedVersion;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
