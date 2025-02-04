/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/lambda/model/ThrottleReason.h>
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
   * <p>The request throughput limit was exceeded. For more information, see <a
   * href="https://docs.aws.amazon.com/lambda/latest/dg/gettingstarted-limits.html#api-requests">Lambda
   * quotas</a>.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/TooManyRequestsException">AWS
   * API Reference</a></p>
   */
  class TooManyRequestsException
  {
  public:
    AWS_LAMBDA_API TooManyRequestsException();
    AWS_LAMBDA_API TooManyRequestsException(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API TooManyRequestsException& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The number of seconds the caller should wait before retrying.</p>
     */
    inline const Aws::String& GetRetryAfterSeconds() const{ return m_retryAfterSeconds; }
    inline bool RetryAfterSecondsHasBeenSet() const { return m_retryAfterSecondsHasBeenSet; }
    inline void SetRetryAfterSeconds(const Aws::String& value) { m_retryAfterSecondsHasBeenSet = true; m_retryAfterSeconds = value; }
    inline void SetRetryAfterSeconds(Aws::String&& value) { m_retryAfterSecondsHasBeenSet = true; m_retryAfterSeconds = std::move(value); }
    inline void SetRetryAfterSeconds(const char* value) { m_retryAfterSecondsHasBeenSet = true; m_retryAfterSeconds.assign(value); }
    inline TooManyRequestsException& WithRetryAfterSeconds(const Aws::String& value) { SetRetryAfterSeconds(value); return *this;}
    inline TooManyRequestsException& WithRetryAfterSeconds(Aws::String&& value) { SetRetryAfterSeconds(std::move(value)); return *this;}
    inline TooManyRequestsException& WithRetryAfterSeconds(const char* value) { SetRetryAfterSeconds(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetType() const{ return m_type; }
    inline bool TypeHasBeenSet() const { return m_typeHasBeenSet; }
    inline void SetType(const Aws::String& value) { m_typeHasBeenSet = true; m_type = value; }
    inline void SetType(Aws::String&& value) { m_typeHasBeenSet = true; m_type = std::move(value); }
    inline void SetType(const char* value) { m_typeHasBeenSet = true; m_type.assign(value); }
    inline TooManyRequestsException& WithType(const Aws::String& value) { SetType(value); return *this;}
    inline TooManyRequestsException& WithType(Aws::String&& value) { SetType(std::move(value)); return *this;}
    inline TooManyRequestsException& WithType(const char* value) { SetType(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetMessage() const{ return m_message; }
    inline bool MessageHasBeenSet() const { return m_messageHasBeenSet; }
    inline void SetMessage(const Aws::String& value) { m_messageHasBeenSet = true; m_message = value; }
    inline void SetMessage(Aws::String&& value) { m_messageHasBeenSet = true; m_message = std::move(value); }
    inline void SetMessage(const char* value) { m_messageHasBeenSet = true; m_message.assign(value); }
    inline TooManyRequestsException& WithMessage(const Aws::String& value) { SetMessage(value); return *this;}
    inline TooManyRequestsException& WithMessage(Aws::String&& value) { SetMessage(std::move(value)); return *this;}
    inline TooManyRequestsException& WithMessage(const char* value) { SetMessage(value); return *this;}
    ///@}

    ///@{
    
    inline const ThrottleReason& GetReason() const{ return m_reason; }
    inline bool ReasonHasBeenSet() const { return m_reasonHasBeenSet; }
    inline void SetReason(const ThrottleReason& value) { m_reasonHasBeenSet = true; m_reason = value; }
    inline void SetReason(ThrottleReason&& value) { m_reasonHasBeenSet = true; m_reason = std::move(value); }
    inline TooManyRequestsException& WithReason(const ThrottleReason& value) { SetReason(value); return *this;}
    inline TooManyRequestsException& WithReason(ThrottleReason&& value) { SetReason(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_retryAfterSeconds;
    bool m_retryAfterSecondsHasBeenSet = false;

    Aws::String m_type;
    bool m_typeHasBeenSet = false;

    Aws::String m_message;
    bool m_messageHasBeenSet = false;

    ThrottleReason m_reason;
    bool m_reasonHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
