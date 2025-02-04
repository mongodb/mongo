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
   * <p>The code signature failed one or more of the validation checks for signature
   * mismatch or expiry, and the code signing policy is set to ENFORCE. Lambda blocks
   * the deployment.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/lambda-2015-03-31/CodeVerificationFailedException">AWS
   * API Reference</a></p>
   */
  class CodeVerificationFailedException
  {
  public:
    AWS_LAMBDA_API CodeVerificationFailedException();
    AWS_LAMBDA_API CodeVerificationFailedException(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API CodeVerificationFailedException& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_LAMBDA_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    
    inline const Aws::String& GetType() const{ return m_type; }
    inline bool TypeHasBeenSet() const { return m_typeHasBeenSet; }
    inline void SetType(const Aws::String& value) { m_typeHasBeenSet = true; m_type = value; }
    inline void SetType(Aws::String&& value) { m_typeHasBeenSet = true; m_type = std::move(value); }
    inline void SetType(const char* value) { m_typeHasBeenSet = true; m_type.assign(value); }
    inline CodeVerificationFailedException& WithType(const Aws::String& value) { SetType(value); return *this;}
    inline CodeVerificationFailedException& WithType(Aws::String&& value) { SetType(std::move(value)); return *this;}
    inline CodeVerificationFailedException& WithType(const char* value) { SetType(value); return *this;}
    ///@}

    ///@{
    
    inline const Aws::String& GetMessage() const{ return m_message; }
    inline bool MessageHasBeenSet() const { return m_messageHasBeenSet; }
    inline void SetMessage(const Aws::String& value) { m_messageHasBeenSet = true; m_message = value; }
    inline void SetMessage(Aws::String&& value) { m_messageHasBeenSet = true; m_message = std::move(value); }
    inline void SetMessage(const char* value) { m_messageHasBeenSet = true; m_message.assign(value); }
    inline CodeVerificationFailedException& WithMessage(const Aws::String& value) { SetMessage(value); return *this;}
    inline CodeVerificationFailedException& WithMessage(Aws::String&& value) { SetMessage(std::move(value)); return *this;}
    inline CodeVerificationFailedException& WithMessage(const char* value) { SetMessage(value); return *this;}
    ///@}
  private:

    Aws::String m_type;
    bool m_typeHasBeenSet = false;

    Aws::String m_message;
    bool m_messageHasBeenSet = false;
  };

} // namespace Model
} // namespace Lambda
} // namespace Aws
