/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/Cors.h>
#include <aws/lambda/model/FunctionUrlAuthType.h>
#include <aws/lambda/model/InvokeMode.h>
#include <utility>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace Lambda
{
namespace Model
{

  /**
   * <p>Details about a Lambda function URL.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/FunctionUrlConfig">AWS
   * API Reference</a></p>
   */
  class FunctionUrlConfig
  {
  public:
    AWS_LAMBDA_API FunctionUrlConfig();
    AWS_LAMBDA_API FunctionUrlConfig(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API FunctionUrlConfig& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The HTTP URL endpoint for your function.</p>
     */
    inline const Aws::String& GetFunctionUrl() const{ return m_functionUrl; }
    inline bool FunctionUrlHasBeenSet() const { return m_functionUrlHasBeenSet; }
    inline void SetFunctionUrl(const Aws::String& value) { m_functionUrlHasBeenSet = true; m_functionUrl = value; }
    inline void SetFunctionUrl(Aws::String&& value) { m_functionUrlHasBeenSet = true; m_functionUrl = std::move(value); }
    inline void SetFunctionUrl(const char* value) { m_functionUrlHasBeenSet = true; m_functionUrl.assign(value); }
    inline FunctionUrlConfig& WithFunctionUrl(const Aws::String& value) { SetFunctionUrl(value); return *this;}
    inline FunctionUrlConfig& WithFunctionUrl(Aws::String&& value) { SetFunctionUrl(std::move(value)); return *this;}
    inline FunctionUrlConfig& WithFunctionUrl(const char* value) { SetFunctionUrl(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of your function.</p>
     */
    inline const Aws::String& GetFunctionArn() const{ return m_functionArn; }
    inline bool FunctionArnHasBeenSet() const { return m_functionArnHasBeenSet; }
    inline void SetFunctionArn(const Aws::String& value) { m_functionArnHasBeenSet = true; m_functionArn = value; }
    inline void SetFunctionArn(Aws::String&& value) { m_functionArnHasBeenSet = true; m_functionArn = std::move(value); }
    inline void SetFunctionArn(const char* value) { m_functionArnHasBeenSet = true; m_functionArn.assign(value); }
    inline FunctionUrlConfig& WithFunctionArn(const Aws::String& value) { SetFunctionArn(value); return *this;}
    inline FunctionUrlConfig& WithFunctionArn(Aws::String&& value) { SetFunctionArn(std::move(value)); return *this;}
    inline FunctionUrlConfig& WithFunctionArn(const char* value) { SetFunctionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When the function URL was created, in <a
     * href="https://www.w3.org/TR/NOTE-datetime">ISO-8601 format</a>
     * (YYYY-MM-DDThh:mm:ss.sTZD).</p>
     */
    inline const Aws::String& GetCreationTime() const{ return m_creationTime; }
    inline bool CreationTimeHasBeenSet() const { return m_creationTimeHasBeenSet; }
    inline void SetCreationTime(const Aws::String& value) { m_creationTimeHasBeenSet = true; m_creationTime = value; }
    inline void SetCreationTime(Aws::String&& value) { m_creationTimeHasBeenSet = true; m_creationTime = std::move(value); }
    inline void SetCreationTime(const char* value) { m_creationTimeHasBeenSet = true; m_creationTime.assign(value); }
    inline FunctionUrlConfig& WithCreationTime(const Aws::String& value) { SetCreationTime(value); return *this;}
    inline FunctionUrlConfig& WithCreationTime(Aws::String&& value) { SetCreationTime(std::move(value)); return *this;}
    inline FunctionUrlConfig& WithCreationTime(const char* value) { SetCreationTime(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>When the function URL configuration was last updated, in <a
     * href="https://www.w3.org/TR/NOTE-datetime">ISO-8601 format</a>
     * (YYYY-MM-DDThh:mm:ss.sTZD).</p>
     */
    inline const Aws::String& GetLastModifiedTime() const{ return m_lastModifiedTime; }
    inline bool LastModifiedTimeHasBeenSet() const { return m_lastModifiedTimeHasBeenSet; }
    inline void SetLastModifiedTime(const Aws::String& value) { m_lastModifiedTimeHasBeenSet = true; m_lastModifiedTime = value; }
    inline void SetLastModifiedTime(Aws::String&& value) { m_lastModifiedTimeHasBeenSet = true; m_lastModifiedTime = std::move(value); }
    inline void SetLastModifiedTime(const char* value) { m_lastModifiedTimeHasBeenSet = true; m_lastModifiedTime.assign(value); }
    inline FunctionUrlConfig& WithLastModifiedTime(const Aws::String& value) { SetLastModifiedTime(value); return *this;}
    inline FunctionUrlConfig& WithLastModifiedTime(Aws::String&& value) { SetLastModifiedTime(std::move(value)); return *this;}
    inline FunctionUrlConfig& WithLastModifiedTime(const char* value) { SetLastModifiedTime(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The <a
     * href="https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS">cross-origin
     * resource sharing (CORS)</a> settings for your function URL.</p>
     */
    inline const Cors& GetCors() const{ return m_cors; }
    inline bool CorsHasBeenSet() const { return m_corsHasBeenSet; }
    inline void SetCors(const Cors& value) { m_corsHasBeenSet = true; m_cors = value; }
    inline void SetCors(Cors&& value) { m_corsHasBeenSet = true; m_cors = std::move(value); }
    inline FunctionUrlConfig& WithCors(const Cors& value) { SetCors(value); return *this;}
    inline FunctionUrlConfig& WithCors(Cors&& value) { SetCors(std::move(value)); return *this;}
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
    inline bool AuthTypeHasBeenSet() const { return m_authTypeHasBeenSet; }
    inline void SetAuthType(const FunctionUrlAuthType& value) { m_authTypeHasBeenSet = true; m_authType = value; }
    inline void SetAuthType(FunctionUrlAuthType&& value) { m_authTypeHasBeenSet = true; m_authType = std::move(value); }
    inline FunctionUrlConfig& WithAuthType(const FunctionUrlAuthType& value) { SetAuthType(value); return *this;}
    inline FunctionUrlConfig& WithAuthType(FunctionUrlAuthType&& value) { SetAuthType(std::move(value)); return *this;}
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
    inline bool InvokeModeHasBeenSet() const { return m_invokeModeHasBeenSet; }
    inline void SetInvokeMode(const InvokeMode& value) { m_invokeModeHasBeenSet = true; m_invokeMode = value; }
    inline void SetInvokeMode(InvokeMode&& value) { m_invokeModeHasBeenSet = true; m_invokeMode = std::move(value); }
    inline FunctionUrlConfig& WithInvokeMode(const InvokeMode& value) { SetInvokeMode(value); return *this;}
    inline FunctionUrlConfig& WithInvokeMode(InvokeMode&& value) { SetInvokeMode(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_functionUrl;
    bool m_functionUrlHasBeenSet = false;

    Aws::String m_functionArn;
    bool m_functionArnHasBeenSet = false;

    Aws::String m_creationTime;
    bool m_creationTimeHasBeenSet = false;

    Aws::String m_lastModifiedTime;
    bool m_lastModifiedTimeHasBeenSet = false;

    Cors m_cors;
    bool m_corsHasBeenSet = false;

    FunctionUrlAuthType m_authType;
    bool m_authTypeHasBeenSet = false;

    InvokeMode m_invokeMode;
    bool m_invokeModeHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
