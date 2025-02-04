/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/RecursiveLoop.h>
#include <utility>

namespace Aws
{
namespace Lambda
{
namespace Model
{

  /**
   */
  class PutFunctionRecursionConfigRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API PutFunctionRecursionConfigRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "PutFunctionRecursionConfig"; }

    AWS_LAMBDA_API Aws::String SerializePayload() const override;


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
    inline PutFunctionRecursionConfigRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline PutFunctionRecursionConfigRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline PutFunctionRecursionConfigRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>If you set your function's recursive loop detection configuration to
     * <code>Allow</code>, Lambda doesn't take any action when it detects your function
     * being invoked as part of a recursive loop. We recommend that you only use this
     * setting if your design intentionally uses a Lambda function to write data back
     * to the same Amazon Web Services resource that invokes it.</p> <p>If you set your
     * function's recursive loop detection configuration to <code>Terminate</code>,
     * Lambda stops your function being invoked and notifies you when it detects your
     * function being invoked as part of a recursive loop.</p> <p>By default, Lambda
     * sets your function's configuration to <code>Terminate</code>.</p> 
     * <p>If your design intentionally uses a Lambda function to write data back to the
     * same Amazon Web Services resource that invokes the function, then use caution
     * and implement suitable guard rails to prevent unexpected charges being billed to
     * your Amazon Web Services account. To learn more about best practices for using
     * recursive invocation patterns, see <a
     * href="https://serverlessland.com/content/service/lambda/guides/aws-lambda-operator-guide/recursive-runaway">Recursive
     * patterns that cause run-away Lambda functions</a> in Serverless Land.</p>
     * 
     */
    inline const RecursiveLoop& GetRecursiveLoop() const{ return m_recursiveLoop; }
    inline bool RecursiveLoopHasBeenSet() const { return m_recursiveLoopHasBeenSet; }
    inline void SetRecursiveLoop(const RecursiveLoop& value) { m_recursiveLoopHasBeenSet = true; m_recursiveLoop = value; }
    inline void SetRecursiveLoop(RecursiveLoop&& value) { m_recursiveLoopHasBeenSet = true; m_recursiveLoop = std::move(value); }
    inline PutFunctionRecursionConfigRequest& WithRecursiveLoop(const RecursiveLoop& value) { SetRecursiveLoop(value); return *this;}
    inline PutFunctionRecursionConfigRequest& WithRecursiveLoop(RecursiveLoop&& value) { SetRecursiveLoop(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    RecursiveLoop m_recursiveLoop;
    bool m_recursiveLoopHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
