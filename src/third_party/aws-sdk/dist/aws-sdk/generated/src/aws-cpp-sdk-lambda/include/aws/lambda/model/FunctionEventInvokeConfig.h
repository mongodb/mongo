/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/DestinationConfig.h>
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

  class FunctionEventInvokeConfig
  {
  public:
    AWS_LAMBDA_API FunctionEventInvokeConfig();
    AWS_LAMBDA_API FunctionEventInvokeConfig(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API FunctionEventInvokeConfig& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The date and time that the configuration was last updated.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModified() const{ return m_lastModified; }
    inline bool LastModifiedHasBeenSet() const { return m_lastModifiedHasBeenSet; }
    inline void SetLastModified(const Aws::Utils::DateTime& value) { m_lastModifiedHasBeenSet = true; m_lastModified = value; }
    inline void SetLastModified(Aws::Utils::DateTime&& value) { m_lastModifiedHasBeenSet = true; m_lastModified = std::move(value); }
    inline FunctionEventInvokeConfig& WithLastModified(const Aws::Utils::DateTime& value) { SetLastModified(value); return *this;}
    inline FunctionEventInvokeConfig& WithLastModified(Aws::Utils::DateTime&& value) { SetLastModified(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the function.</p>
     */
    inline const Aws::String& GetFunctionArn() const{ return m_functionArn; }
    inline bool FunctionArnHasBeenSet() const { return m_functionArnHasBeenSet; }
    inline void SetFunctionArn(const Aws::String& value) { m_functionArnHasBeenSet = true; m_functionArn = value; }
    inline void SetFunctionArn(Aws::String&& value) { m_functionArnHasBeenSet = true; m_functionArn = std::move(value); }
    inline void SetFunctionArn(const char* value) { m_functionArnHasBeenSet = true; m_functionArn.assign(value); }
    inline FunctionEventInvokeConfig& WithFunctionArn(const Aws::String& value) { SetFunctionArn(value); return *this;}
    inline FunctionEventInvokeConfig& WithFunctionArn(Aws::String&& value) { SetFunctionArn(std::move(value)); return *this;}
    inline FunctionEventInvokeConfig& WithFunctionArn(const char* value) { SetFunctionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of times to retry when the function returns an error.</p>
     */
    inline int GetMaximumRetryAttempts() const{ return m_maximumRetryAttempts; }
    inline bool MaximumRetryAttemptsHasBeenSet() const { return m_maximumRetryAttemptsHasBeenSet; }
    inline void SetMaximumRetryAttempts(int value) { m_maximumRetryAttemptsHasBeenSet = true; m_maximumRetryAttempts = value; }
    inline FunctionEventInvokeConfig& WithMaximumRetryAttempts(int value) { SetMaximumRetryAttempts(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum age of a request that Lambda sends to a function for
     * processing.</p>
     */
    inline int GetMaximumEventAgeInSeconds() const{ return m_maximumEventAgeInSeconds; }
    inline bool MaximumEventAgeInSecondsHasBeenSet() const { return m_maximumEventAgeInSecondsHasBeenSet; }
    inline void SetMaximumEventAgeInSeconds(int value) { m_maximumEventAgeInSecondsHasBeenSet = true; m_maximumEventAgeInSeconds = value; }
    inline FunctionEventInvokeConfig& WithMaximumEventAgeInSeconds(int value) { SetMaximumEventAgeInSeconds(value); return *this;}
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
    inline FunctionEventInvokeConfig& WithDestinationConfig(const DestinationConfig& value) { SetDestinationConfig(value); return *this;}
    inline FunctionEventInvokeConfig& WithDestinationConfig(DestinationConfig&& value) { SetDestinationConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline bool RequestIdHasBeenSet() const { return m_requestIdHasBeenSet; }
    inline void SetRequestId(const Aws::String& value) { m_requestIdHasBeenSet = true; m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestIdHasBeenSet = true; m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestIdHasBeenSet = true; m_requestId.assign(value); }
    inline FunctionEventInvokeConfig& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline FunctionEventInvokeConfig& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline FunctionEventInvokeConfig& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Utils::DateTime m_lastModified;
    bool m_lastModifiedHasBeenSet = false;

    Aws::String m_functionArn;
    bool m_functionArnHasBeenSet = false;

    int m_maximumRetryAttempts;
    bool m_maximumRetryAttemptsHasBeenSet = false;

    int m_maximumEventAgeInSeconds;
    bool m_maximumEventAgeInSecondsHasBeenSet = false;

    DestinationConfig m_destinationConfig;
    bool m_destinationConfigHasBeenSet = false;

    Aws::String m_requestId;
    bool m_requestIdHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
