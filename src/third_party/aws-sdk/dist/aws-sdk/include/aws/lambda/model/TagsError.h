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
   * <p>An object that contains details about an error related to retrieving
   * tags.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/TagsError">AWS
   * API Reference</a></p>
   */
  class TagsError
  {
  public:
    AWS_LAMBDA_API TagsError();
    AWS_LAMBDA_API TagsError(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API TagsError& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>The error code.</p>
     */
    inline const Aws::String& GetErrorCode() const{ return m_errorCode; }
    inline bool ErrorCodeHasBeenSet() const { return m_errorCodeHasBeenSet; }
    inline void SetErrorCode(const Aws::String& value) { m_errorCodeHasBeenSet = true; m_errorCode = value; }
    inline void SetErrorCode(Aws::String&& value) { m_errorCodeHasBeenSet = true; m_errorCode = std::move(value); }
    inline void SetErrorCode(const char* value) { m_errorCodeHasBeenSet = true; m_errorCode.assign(value); }
    inline TagsError& WithErrorCode(const Aws::String& value) { SetErrorCode(value); return *this;}
    inline TagsError& WithErrorCode(Aws::String&& value) { SetErrorCode(std::move(value)); return *this;}
    inline TagsError& WithErrorCode(const char* value) { SetErrorCode(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The error message.</p>
     */
    inline const Aws::String& GetMessage() const{ return m_message; }
    inline bool MessageHasBeenSet() const { return m_messageHasBeenSet; }
    inline void SetMessage(const Aws::String& value) { m_messageHasBeenSet = true; m_message = value; }
    inline void SetMessage(Aws::String&& value) { m_messageHasBeenSet = true; m_message = std::move(value); }
    inline void SetMessage(const char* value) { m_messageHasBeenSet = true; m_message.assign(value); }
    inline TagsError& WithMessage(const Aws::String& value) { SetMessage(value); return *this;}
    inline TagsError& WithMessage(Aws::String&& value) { SetMessage(std::move(value)); return *this;}
    inline TagsError& WithMessage(const char* value) { SetMessage(value); return *this;}
    ///@}
  private:

    Aws::String m_errorCode;
    bool m_errorCodeHasBeenSet = false;

    Aws::String m_message;
    bool m_messageHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
