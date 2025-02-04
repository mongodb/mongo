/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/FunctionUrlAuthType.h>
#include <aws/lambda/model/Cors.h>
#include <aws/lambda/model/InvokeMode.h>
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
  class UpdateFunctionUrlConfigResult
  {
  public:
    AWS_LAMBDA_API UpdateFunctionUrlConfigResult();
    AWS_LAMBDA_API UpdateFunctionUrlConfigResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API UpdateFunctionUrlConfigResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The HTTP URL endpoint for your function.</p>
     */
    inline const Aws::String& GetFunctionUrl() const{ return m_functionUrl; }
    inline void SetFunctionUrl(const Aws::String& value) { m_functionUrl = value; }
    inline void SetFunctionUrl(Aws::String&& value) { m_functionUrl = std::move(value); }
    inline void SetFunctionUrl(const char* value) { m_functionUrl.assign(value); }
    inline UpdateFunctionUrlConfigResult& WithFunctionUrl(const Aws::String& value) { SetFunctionUrl(value); return *this;}
    inline UpdateFunctionUrlConfigResult& WithFunctionUrl(Aws::String&& value) { SetFunctionUrl(std::move(value)); return *this;}
    inline UpdateFunctionUrlConfigResult& WithFunctionUrl(const char* value) { SetFunctionUrl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of your function.</p>
     */
    inline const Aws::String& GetFunctionArn() const{ return m_functionArn; }
    inline void SetFunctionArn(const Aws::String& value) { m_functionArn = value; }
    inline void SetFunctionArn(Aws::String&& value) { m_functionArn = std::move(value); }
    inline void SetFunctionArn(const char* value) { m_functionArn.assign(value); }
    inline UpdateFunctionUrlConfigResult& WithFunctionArn(const Aws::String& value) { SetFunctionArn(value); return *this;}
    inline UpdateFunctionUrlConfigResult& WithFunctionArn(Aws::String&& value) { SetFunctionArn(std::move(value)); return *this;}
    inline UpdateFunctionUrlConfigResult& WithFunctionArn(const char* value) { SetFunctionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The type of authentication that your function URL uses. Set to
     * <code>AWS_IAM</code> if you want to restrict access to authenticated users only.
     * Set to <code>NONE</code> if you want to bypass IAM authentication to create a
     * public endpoint. For more information, see <a
     * href="https://docs.aws.amazon.com/lambda/latest/dg/urls-auth.html">Security and
     * auth model for Lambda function URLs</a>.</p>
     */
    inline const FunctionUrlAuthType& GetAuthType() const{ return m_authType; }
    inline void SetAuthType(const FunctionUrlAuthType& value) { m_authType = value; }
    inline void SetAuthType(FunctionUrlAuthType&& value) { m_authType = std::move(value); }
    inline UpdateFunctionUrlConfigResult& WithAuthType(const FunctionUrlAuthType& value) { SetAuthType(value); return *this;}
    inline UpdateFunctionUrlConfigResult& WithAuthType(FunctionUrlAuthType&& value) { SetAuthType(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The <a
     * href="https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS">cross-origin
     * resource sharing (CORS)</a> settings for your function URL.</p>
     */
    inline const Cors& GetCors() const{ return m_cors; }
    inline void SetCors(const Cors& value) { m_cors = value; }
    inline void SetCors(Cors&& value) { m_cors = std::move(value); }
    inline UpdateFunctionUrlConfigResult& WithCors(const Cors& value) { SetCors(value); return *this;}
    inline UpdateFunctionUrlConfigResult& WithCors(Cors&& value) { SetCors(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>When the function URL was created, in <a
     * href="https://www.w3.org/TR/NOTE-datetime">ISO-8601 format</a>
     * (YYYY-MM-DDThh:mm:ss.sTZD).</p>
     */
    inline const Aws::String& GetCreationTime() const{ return m_creationTime; }
    inline void SetCreationTime(const Aws::String& value) { m_creationTime = value; }
    inline void SetCreationTime(Aws::String&& value) { m_creationTime = std::move(value); }
    inline void SetCreationTime(const char* value) { m_creationTime.assign(value); }
    inline UpdateFunctionUrlConfigResult& WithCreationTime(const Aws::String& value) { SetCreationTime(value); return *this;}
    inline UpdateFunctionUrlConfigResult& WithCreationTime(Aws::String&& value) { SetCreationTime(std::move(value)); return *this;}
    inline UpdateFunctionUrlConfigResult& WithCreationTime(const char* value) { SetCreationTime(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When the function URL configuration was last updated, in <a
     * href="https://www.w3.org/TR/NOTE-datetime">ISO-8601 format</a>
     * (YYYY-MM-DDThh:mm:ss.sTZD).</p>
     */
    inline const Aws::String& GetLastModifiedTime() const{ return m_lastModifiedTime; }
    inline void SetLastModifiedTime(const Aws::String& value) { m_lastModifiedTime = value; }
    inline void SetLastModifiedTime(Aws::String&& value) { m_lastModifiedTime = std::move(value); }
    inline void SetLastModifiedTime(const char* value) { m_lastModifiedTime.assign(value); }
    inline UpdateFunctionUrlConfigResult& WithLastModifiedTime(const Aws::String& value) { SetLastModifiedTime(value); return *this;}
    inline UpdateFunctionUrlConfigResult& WithLastModifiedTime(Aws::String&& value) { SetLastModifiedTime(std::move(value)); return *this;}
    inline UpdateFunctionUrlConfigResult& WithLastModifiedTime(const char* value) { SetLastModifiedTime(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>Use one of the following options:</p> <ul> <li> <p> <code>BUFFERED</code> –
     * This is the default option. Lambda invokes your function using the
     * <code>Invoke</code> API operation. Invocation results are available when the
     * payload is complete. The maximum payload size is 6 MB.</p> </li> <li> <p>
     * <code>RESPONSE_STREAM</code> – Your function streams payload results as they
     * become available. Lambda invokes your function using the
     * <code>InvokeWithResponseStream</code> API operation. The maximum response
     * payload size is 20 MB, however, you can <a
     * href="https://docs.aws.amazon.com/servicequotas/latest/userguide/request-quota-increase.html">request
     * a quota increase</a>.</p> </li> </ul>
     */
    inline const InvokeMode& GetInvokeMode() const{ return m_invokeMode; }
    inline void SetInvokeMode(const InvokeMode& value) { m_invokeMode = value; }
    inline void SetInvokeMode(InvokeMode&& value) { m_invokeMode = std::move(value); }
    inline UpdateFunctionUrlConfigResult& WithInvokeMode(const InvokeMode& value) { SetInvokeMode(value); return *this;}
    inline UpdateFunctionUrlConfigResult& WithInvokeMode(InvokeMode&& value) { SetInvokeMode(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline UpdateFunctionUrlConfigResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline UpdateFunctionUrlConfigResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline UpdateFunctionUrlConfigResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::String m_functionUrl;

    Aws::String m_functionArn;

    FunctionUrlAuthType m_authType;

    Cors m_cors;

    Aws::String m_creationTime;

    Aws::String m_lastModifiedTime;

    InvokeMode m_invokeMode;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
