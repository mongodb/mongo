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
   * <p>The <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-async.html#dlq">dead-letter
   * queue</a> for failed asynchronous invocations.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/DeadLetterConfig">AWS
   * API Reference</a></p>
   */
  class DeadLetterConfig
  {
  public:
    AWS_LAMBDA_API DeadLetterConfig();
    AWS_LAMBDA_API DeadLetterConfig(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API DeadLetterConfig& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of an Amazon SQS queue or Amazon SNS
     * topic.</p>
     */
    inline const Aws::String& GetTargetArn() const{ return m_targetArn; }
    inline bool TargetArnHasBeenSet() const { return m_targetArnHasBeenSet; }
    inline void SetTargetArn(const Aws::String& value) { m_targetArnHasBeenSet = true; m_targetArn = value; }
    inline void SetTargetArn(Aws::String&& value) { m_targetArnHasBeenSet = true; m_targetArn = std::move(value); }
    inline void SetTargetArn(const char* value) { m_targetArnHasBeenSet = true; m_targetArn.assign(value); }
    inline DeadLetterConfig& WithTargetArn(const Aws::String& value) { SetTargetArn(value); return *this;}
    inline DeadLetterConfig& WithTargetArn(Aws::String&& value) { SetTargetArn(std::move(value)); return *this;}
    inline DeadLetterConfig& WithTargetArn(const char* value) { SetTargetArn(value); return *this;}
    ///@}
  private:

    Aws::String m_targetArn;
    bool m_targetArnHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
