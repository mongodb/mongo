/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/FunctionUrlAuthType.h>
#include <aws/lambda/model/Cors.h>
#include <aws/lambda/model/InvokeMode.h>
#include <utility>

namespace Aws
{
namespace Http
{
    class URI;
} //namespace Http
namespace Lambda
{
namespace Model
{

  /**
   */
  class UpdateFunctionUrlConfigRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API UpdateFunctionUrlConfigRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateFunctionUrlConfig"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;

    AWS_LAMBDA_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;


    ///@{
    /**
     * <p>The name or ARN of the Lambda function.</p> <p class="title"> <b>Name
     * formats</b> </p> <ul> <li> <p> <b>Function name</b> –
     * <code>my-function</code>.</p> </li> <li> <p> <b>Function ARN</b> –
     * <code>arn:aws:lambda:us-west-2:123456789012:function:my-function</code>.</p>
     * </li> <li> <p> <b>Partial ARN</b> –
     * <code>123456789012:function:my-function</code>.</p> </li> </ul> <p>The length
     * constraint applies only to the full ARN. If you specify only the function name,
     * it is limited to 64 characters in length.</p>
     */
    inline const Aws::String& GetFunctionName() const{ return m_functionName; }
    inline bool FunctionNameHasBeenSet() const { return m_functionNameHasBeenSet; }
    inline void SetFunctionName(const Aws::String& value) { m_functionNameHasBeenSet = true; m_functionName = value; }
    inline void SetFunctionName(Aws::String&& value) { m_functionNameHasBeenSet = true; m_functionName = std::move(value); }
    inline void SetFunctionName(const char* value) { m_functionNameHasBeenSet = true; m_functionName.assign(value); }
    inline UpdateFunctionUrlConfigRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline UpdateFunctionUrlConfigRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline UpdateFunctionUrlConfigRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The alias name.</p>
     */
    inline const Aws::String& GetQualifier() const{ return m_qualifier; }
    inline bool QualifierHasBeenSet() const { return m_qualifierHasBeenSet; }
    inline void SetQualifier(const Aws::String& value) { m_qualifierHasBeenSet = true; m_qualifier = value; }
    inline void SetQualifier(Aws::String&& value) { m_qualifierHasBeenSet = true; m_qualifier = std::move(value); }
    inline void SetQualifier(const char* value) { m_qualifierHasBeenSet = true; m_qualifier.assign(value); }
    inline UpdateFunctionUrlConfigRequest& WithQualifier(const Aws::String& value) { SetQualifier(value); return *this;}
    inline UpdateFunctionUrlConfigRequest& WithQualifier(Aws::String&& value) { SetQualifier(std::move(value)); return *this;}
    inline UpdateFunctionUrlConfigRequest& WithQualifier(const char* value) { SetQualifier(value); return *this;}
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
    inline UpdateFunctionUrlConfigRequest& WithAuthType(const FunctionUrlAuthType& value) { SetAuthType(value); return *this;}
    inline UpdateFunctionUrlConfigRequest& WithAuthType(FunctionUrlAuthType&& value) { SetAuthType(std::move(value)); return *this;}
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
    inline UpdateFunctionUrlConfigRequest& WithCors(const Cors& value) { SetCors(value); return *this;}
    inline UpdateFunctionUrlConfigRequest& WithCors(Cors&& value) { SetCors(std::move(value)); return *this;}
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
    inline UpdateFunctionUrlConfigRequest& WithInvokeMode(const InvokeMode& value) { SetInvokeMode(value); return *this;}
    inline UpdateFunctionUrlConfigRequest& WithInvokeMode(InvokeMode&& value) { SetInvokeMode(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    Aws::String m_qualifier;
    bool m_qualifierHasBeenSet = false;

    FunctionUrlAuthType m_authType;
    bool m_authTypeHasBeenSet = false;

    Cors m_cors;
    bool m_corsHasBeenSet = false;

    InvokeMode m_invokeMode;
    bool m_invokeModeHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
