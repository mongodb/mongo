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
   * <p>A response confirming that the event stream is complete.</p><p><h3>See
   * Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/InvokeWithResponseStreamCompleteEvent">AWS
   * API Reference</a></p>
   */
  class InvokeWithResponseStreamCompleteEvent
  {
  public:
    AWS_LAMBDA_API InvokeWithResponseStreamCompleteEvent();
    AWS_LAMBDA_API InvokeWithResponseStreamCompleteEvent(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API InvokeWithResponseStreamCompleteEvent& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>An error code.</p>
     */
    inline const Aws::String& GetErrorCode() const{ return m_errorCode; }
    inline bool ErrorCodeHasBeenSet() const { return m_errorCodeHasBeenSet; }
    inline void SetErrorCode(const Aws::String& value) { m_errorCodeHasBeenSet = true; m_errorCode = value; }
    inline void SetErrorCode(Aws::String&& value) { m_errorCodeHasBeenSet = true; m_errorCode = std::move(value); }
    inline void SetErrorCode(const char* value) { m_errorCodeHasBeenSet = true; m_errorCode.assign(value); }
    inline InvokeWithResponseStreamCompleteEvent& WithErrorCode(const Aws::String& value) { SetErrorCode(value); return *this;}
    inline InvokeWithResponseStreamCompleteEvent& WithErrorCode(Aws::String&& value) { SetErrorCode(std::move(value)); return *this;}
    inline InvokeWithResponseStreamCompleteEvent& WithErrorCode(const char* value) { SetErrorCode(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The details of any returned error.</p>
     */
    inline const Aws::String& GetErrorDetails() const{ return m_errorDetails; }
    inline bool ErrorDetailsHasBeenSet() const { return m_errorDetailsHasBeenSet; }
    inline void SetErrorDetails(const Aws::String& value) { m_errorDetailsHasBeenSet = true; m_errorDetails = value; }
    inline void SetErrorDetails(Aws::String&& value) { m_errorDetailsHasBeenSet = true; m_errorDetails = std::move(value); }
    inline void SetErrorDetails(const char* value) { m_errorDetailsHasBeenSet = true; m_errorDetails.assign(value); }
    inline InvokeWithResponseStreamCompleteEvent& WithErrorDetails(const Aws::String& value) { SetErrorDetails(value); return *this;}
    inline InvokeWithResponseStreamCompleteEvent& WithErrorDetails(Aws::String&& value) { SetErrorDetails(std::move(value)); return *this;}
    inline InvokeWithResponseStreamCompleteEvent& WithErrorDetails(const char* value) { SetErrorDetails(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The last 4 KB of the execution log, which is base64-encoded.</p>
     */
    inline const Aws::String& GetLogResult() const{ return m_logResult; }
    inline bool LogResultHasBeenSet() const { return m_logResultHasBeenSet; }
    inline void SetLogResult(const Aws::String& value) { m_logResultHasBeenSet = true; m_logResult = value; }
    inline void SetLogResult(Aws::String&& value) { m_logResultHasBeenSet = true; m_logResult = std::move(value); }
    inline void SetLogResult(const char* value) { m_logResultHasBeenSet = true; m_logResult.assign(value); }
    inline InvokeWithResponseStreamCompleteEvent& WithLogResult(const Aws::String& value) { SetLogResult(value); return *this;}
    inline InvokeWithResponseStreamCompleteEvent& WithLogResult(Aws::String&& value) { SetLogResult(std::move(value)); return *this;}
    inline InvokeWithResponseStreamCompleteEvent& WithLogResult(const char* value) { SetLogResult(value); return *this;}
    ///@}
  private:

    Aws::String m_errorCode;
    bool m_errorCodeHasBeenSet = false;

    Aws::String m_errorDetails;
    bool m_errorDetailsHasBeenSet = false;

    Aws::String m_logResult;
    bool m_logResultHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
