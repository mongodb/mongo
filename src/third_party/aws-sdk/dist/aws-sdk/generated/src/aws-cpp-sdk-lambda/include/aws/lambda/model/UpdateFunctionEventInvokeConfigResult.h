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
  class UpdateFunctionEventInvokeConfigResult
  {
  public:
    AWS_LAMBDA_API UpdateFunctionEventInvokeConfigResult();
    AWS_LAMBDA_API UpdateFunctionEventInvokeConfigResult(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);
    AWS_LAMBDA_API UpdateFunctionEventInvokeConfigResult& operator=(const Aws::AmazonWebServiceResult<Aws::Utils::Json::JsonValue>& result);


    ///@{
    /**
     * <p>The date and time that the configuration was last updated.</p>
     */
    inline const Aws::Utils::DateTime& GetLastModified() const{ return m_lastModified; }
    inline void SetLastModified(const Aws::Utils::DateTime& value) { m_lastModified = value; }
    inline void SetLastModified(Aws::Utils::DateTime&& value) { m_lastModified = std::move(value); }
    inline UpdateFunctionEventInvokeConfigResult& WithLastModified(const Aws::Utils::DateTime& value) { SetLastModified(value); return *this;}
    inline UpdateFunctionEventInvokeConfigResult& WithLastModified(Aws::Utils::DateTime&& value) { SetLastModified(std::move(value)); return *this;}
    ///@}

    ///@{
    /**
     * <p>The Amazon Resource Name (ARN) of the function.</p>
     */
    inline const Aws::String& GetFunctionArn() const{ return m_functionArn; }
    inline void SetFunctionArn(const Aws::String& value) { m_functionArn = value; }
    inline void SetFunctionArn(Aws::String&& value) { m_functionArn = std::move(value); }
    inline void SetFunctionArn(const char* value) { m_functionArn.assign(value); }
    inline UpdateFunctionEventInvokeConfigResult& WithFunctionArn(const Aws::String& value) { SetFunctionArn(value); return *this;}
    inline UpdateFunctionEventInvokeConfigResult& WithFunctionArn(Aws::String&& value) { SetFunctionArn(std::move(value)); return *this;}
    inline UpdateFunctionEventInvokeConfigResult& WithFunctionArn(const char* value) { SetFunctionArn(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum number of times to retry when the function returns an error.</p>
     */
    inline int GetMaximumRetryAttempts() const{ return m_maximumRetryAttempts; }
    inline void SetMaximumRetryAttempts(int value) { m_maximumRetryAttempts = value; }
    inline UpdateFunctionEventInvokeConfigResult& WithMaximumRetryAttempts(int value) { SetMaximumRetryAttempts(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The maximum age of a request that Lambda sends to a function for
     * processing.</p>
     */
    inline int GetMaximumEventAgeInSeconds() const{ return m_maximumEventAgeInSeconds; }
    inline void SetMaximumEventAgeInSeconds(int value) { m_maximumEventAgeInSeconds = value; }
    inline UpdateFunctionEventInvokeConfigResult& WithMaximumEventAgeInSeconds(int value) { SetMaximumEventAgeInSeconds(value); return *this;}
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
    inline void SetDestinationConfig(const DestinationConfig& value) { m_destinationConfig = value; }
    inline void SetDestinationConfig(DestinationConfig&& value) { m_destinationConfig = std::move(value); }
    inline UpdateFunctionEventInvokeConfigResult& WithDestinationConfig(const DestinationConfig& value) { SetDestinationConfig(value); return *this;}
    inline UpdateFunctionEventInvokeConfigResult& WithDestinationConfig(DestinationConfig&& value) { SetDestinationConfig(std::move(value)); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetRequestId() const{ return m_requestId; }
    inline void SetRequestId(const Aws::String& value) { m_requestId = value; }
    inline void SetRequestId(Aws::String&& value) { m_requestId = std::move(value); }
    inline void SetRequestId(const char* value) { m_requestId.assign(value); }
    inline UpdateFunctionEventInvokeConfigResult& WithRequestId(const Aws::String& value) { SetRequestId(value); return *this;}
    inline UpdateFunctionEventInvokeConfigResult& WithRequestId(Aws::String&& value) { SetRequestId(std::move(value)); return *this;}
    inline UpdateFunctionEventInvokeConfigResult& WithRequestId(const char* value) { SetRequestId(value); return *this;}
    ///@}
  private:

    Aws::Utils::DateTime m_lastModified;

    Aws::String m_functionArn;

    int m_maximumRetryAttempts;

    int m_maximumEventAgeInSeconds;

    DestinationConfig m_destinationConfig;

    Aws::String m_requestId;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
