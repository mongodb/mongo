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
   * <p>A destination for events that were processed successfully.</p> <p>To retain
   * records of successful <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/invocation-async.html#invocation-async-destinations">asynchronous
   * invocations</a>, you can configure an Amazon SNS topic, Amazon SQS queue, Lambda
   * function, or Amazon EventBridge event bus as the destination.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/OnSuccess">AWS
   * API Reference</a></p>
   */
  class OnSuccess
  {
  public:
    AWS_LAMBDA_API OnSuccess();
    AWS_LAMBDA_API OnSuccess(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API OnSuccess& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the destination resource.</p>
     */
    inline const Aws::String& GetDestination() const{ return m_destination; }
    inline bool DestinationHasBeenSet() const { return m_destinationHasBeenSet; }
    inline void SetDestination(const Aws::String& value) { m_destinationHasBeenSet = true; m_destination = value; }
    inline void SetDestination(Aws::String&& value) { m_destinationHasBeenSet = true; m_destination = std::move(value); }
    inline void SetDestination(const char* value) { m_destinationHasBeenSet = true; m_destination.assign(value); }
    inline OnSuccess& WithDestination(const Aws::String& value) { SetDestination(value); return *this;}
    inline OnSuccess& WithDestination(Aws::String&& value) { SetDestination(std::move(value)); return *this;}
    inline OnSuccess& WithDestination(const char* value) { SetDestination(value); return *this;}
    ///@}
  private:

    Aws::String m_destination;
    bool m_destinationHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
