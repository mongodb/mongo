/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaRequest.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <utility>

namespace Aws
{
namespace Lambda
{
namespace Model
{

  /**
   */
  class PutFunctionConcurrencyRequest : public LambdaRequest
  {
  public:
    AWS_LAMBDA_API PutFunctionConcurrencyRequest();

    // Service request name is the Operation name which will send this request out,
    // each operation should has unique request name, so that we can get operation's name from this request.
    // Note: this is not true for response, multiple operations may have the same response name,
    // so we can not get operation's name from response.
    inline virtual const char* GetServiceRequestName() const override { return "PutFunctionConcurrency"; }

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
    inline PutFunctionConcurrencyRequest& WithFunctionName(const Aws::String& value) { SetFunctionName(value); return *this;}
    inline PutFunctionConcurrencyRequest& WithFunctionName(Aws::String&& value) { SetFunctionName(std::move(value)); return *this;}
    inline PutFunctionConcurrencyRequest& WithFunctionName(const char* value) { SetFunctionName(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The number of simultaneous executions to reserve for the function.</p>
     */
    inline int GetReservedConcurrentExecutions() const{ return m_reservedConcurrentExecutions; }
    inline bool ReservedConcurrentExecutionsHasBeenSet() const { return m_reservedConcurrentExecutionsHasBeenSet; }
    inline void SetReservedConcurrentExecutions(int value) { m_reservedConcurrentExecutionsHasBeenSet = true; m_reservedConcurrentExecutions = value; }
    inline PutFunctionConcurrencyRequest& WithReservedConcurrentExecutions(int value) { SetReservedConcurrentExecutions(value); return *this;}
    ///@}
  private:

    Aws::String m_functionName;
    bool m_functionNameHasBeenSet = false;

    int m_reservedConcurrentExecutions;
    bool m_reservedConcurrentExecutionsHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
