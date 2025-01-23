/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/DestinationConfig.h>
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
  class UpdateFunctionEventInvokeConfigRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API UpdateFunctionEventInvokeConfigRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "UpdateFunctionEventInvokeConfig"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;

    AWS_LAMBDA_API void AddQueryStringParameters(Aws::Http::URI& uri) const override;


    ///@{
    /**
     * <p>The name or ARN of the Lambda function, version, or alias.</p> <p
     * class="title"> <b>Name formats</b> </p> <ul> <li> <p> <b>Function name</b> -
     * <code>my-function</code> (name-only), <code>my-function:v1</code> (with
     * alias).</p> </li> <li> <p> <b>Function ARN</b> -
     * <code>arn:aws:lambda:us-west-2:123456789012:function:my-function</code>.</p>
     * </li> <li> <p> <b>Partial ARN</b> -
     * <code>123456789012:function:my-function</code>.</p> </li> </ul> <p>You can
     * append a version number or alias to any of the formats. The length constraint
     * applies only to the full ARN. If you specify only the function name, it is
     * limited to 64 characters in length.</p>
     */
    inline const Aws::String& GetFunctionName() const{ return m_functionName; }
    inline bool FunctionNameHasBeenSet() const { return m_functionNameHasBeenSet; }
    inline void SetFunctionName(const Aws::String& value) { m_functionNameHasBeenSet = true; m_functionName = value; }
    inline void SetFunctionName(Aws::String&& value) { m_functionNameHasBeenSet = true; m_functionName = std::move(value); }
    inline void SetFunctionName(const char* value) { m_functionNameHasBeenSet = true; m_functionName.assign(value); }
    inline UpdateFunctionEventInvokeConfigRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline UpdateFunctionEventInvokeConfigRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline UpdateFunctionEventInvokeConfigRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A version number or alias name.</p>
     */
    inline const Aws::String& GetQualifier() const{ return m_qualifier; }
    inline bool QualifierHasBeenSet() const { return m_qualifierHasBeenSet; }
    inline void SetQualifier(const Aws::String& value) { m_qualifierHasBeenSet = true; m_qualifier = value; }
    inline void SetQualifier(Aws::String&& value) { m_qualifierHasBeenSet = true; m_qualifier = std::move(value); }
    inline void SetQualifier(const char* value) { m_qualifierHasBeenSet = true; m_qualifier.assign(value); }
    inline UpdateFunctionEventInvokeConfigRequest& WithQualifier(const Aws::String& value) { SetQualifier(value); return *this;}
    inline UpdateFunctionEventInvokeConfigRequest& WithQualifier(Aws::String&& value) { SetQualifier(std::move(value)); return *this;}
    inline UpdateFunctionEventInvokeConfigRequest& WithQualifier(const char* value) { SetQualifier(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of times to retry when the function returns an error.</p>
     */
    inline int GetMaximumRetryAttempts() const{ return m_maximumRetryAttempts; }
    inline bool MaximumRetryAttemptsHasBeenSet() const { return m_maximumRetryAttemptsHasBeenSet; }
    inline void SetMaximumRetryAttempts(int value) { m_maximumRetryAttemptsHasBeenSet = true; m_maximumRetryAttempts = value; }
    inline UpdateFunctionEventInvokeConfigRequest& WithMaximumRetryAttempts(int value) { SetMaximumRetryAttempts(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum age of a request that Lambda sends to a function for
     * processing.</p>
     */
    inline int GetMaximumEventAgeInSeconds() const{ return m_maximumEventAgeInSeconds; }
    inline bool MaximumEventAgeInSecondsHasBeenSet() const { return m_maximumEventAgeInSecondsHasBeenSet; }
    inline void SetMaximumEventAgeInSeconds(int value) { m_maximumEventAgeInSecondsHasBeenSet = true; m_maximumEventAgeInSeconds = value; }
    inline UpdateFunctionEventInvokeConfigRequest& WithMaximumEventAgeInSeconds(int value) { SetMaximumEventAgeInSeconds(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>A destination for events after they have been sent to a function for
     * processing.</p> <p class="title"> <b>Destinations</b> </p> <ul> <li> <p>
     * <b>Function</b> - The Amazon Resource Name (ARN) of a Lambda function.</p> </li>
     * <li> <p> <b>Queue</b> - The ARN of a standard SQS queue.</p> </li> <li> <p>
     * <b>Bucket</b> - The ARN of an Amazon S3 bucket.</p> </li> <li> <p> <b>Topic</b>
     * - The ARN of a standard SNS topic.</p> </li> <li> <p> <b>Event Bus</b> - The ARN
     * of an Amazon EventBridge event bus.</p> </li> </ul>  <p>S3 buckets are
     * supported only for on-failure destinations. To retain records of successful
     * invocations, use another destination type.</p> 
     */
    inline const DestinationConfig& GetDestinationConfig() const{ return m_destinationConfig; }
    inline bool DestinationConfigHasBeenSet() const { return m_destinationConfigHasBeenSet; }
    inline void SetDestinationConfig(const DestinationConfig& value) { m_destinationConfigHasBeenSet = true; m_destinationConfig = value; }
    inline void SetDestinationConfig(DestinationConfig&& value) { m_destinationConfigHasBeenSet = true; m_destinationConfig = std::move(value); }
    inline UpdateFunctionEventInvokeConfigRequest& WithDestinationConfig(const DestinationConfig& value) { SetDestinationConfig(value); return *this;}
    inline UpdateFunctionEventInvokeConfigRequest& WithDestinationConfig(DestinationConfig&& value) { SetDestinationConfig(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    Aws::String m_qualifier;
    bool m_qualifierHasBeenSet = false;

    int m_maximumRetryAttempts;
    bool m_maximumRetryAttemptsHasBeenSet = false;

    int m_maximumEventAgeInSeconds;
    bool m_maximumEventAgeInSecondsHasBeenSet = false;

    DestinationConfig m_destinationConfig;
    bool m_destinationConfigHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
