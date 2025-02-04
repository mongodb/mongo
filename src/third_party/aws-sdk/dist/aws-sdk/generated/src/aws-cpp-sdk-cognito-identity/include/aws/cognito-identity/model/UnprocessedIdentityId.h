/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/cognito-identity/model/ErrorCode.h>
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
namespace CognitoIdentity
{
namespace Model
{

  /**
   * <p>An array of UnprocessedIdentityId objects, each of which contains an
   * ErrorCode and IdentityId.</p><p><h3>See Also:</h3>   <a
   * href="http://docs.aws.amazon.com/goto/WebAPI/cognito-identity-2014-06-30/UnprocessedIdentityId">AWS
   * API Reference</a></p>
   */
  class UnprocessedIdentityId
  {
  public:
    AWS_COGNITOIDENTITY_API UnprocessedIdentityId();
    AWS_COGNITOIDENTITY_API UnprocessedIdentityId(Aws::Utils::Json::JsonView jsonValue);
    AWS_COGNITOIDENTITY_API UnprocessedIdentityId& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_COGNITOIDENTITY_API Aws::Utils::Json::JsonValue Jsonize() const;


    ///@{
    /**
     * <p>A unique identifier in the format REGION:GUID.</p>
     */
    inline const Aws::String& GetIdentityId() const{ return m_identityId; }
    inline bool IdentityIdHasBeenSet() const { return m_identityIdHasBeenSet; }
    inline void SetIdentityId(const Aws::String& value) { m_identityIdHasBeenSet = true; m_identityId = value; }
    inline void SetIdentityId(Aws::String&& value) { m_identityIdHasBeenSet = true; m_identityId = std::move(value); }
    inline void SetIdentityId(const char* value) { m_identityIdHasBeenSet = true; m_identityId.assign(value); }
    inline UnprocessedIdentityId& WithIdentityId(const Aws::String& value) { SetIdentityId(value); return *this;}
    inline UnprocessedIdentityId& WithIdentityId(Aws::String&& value) { SetIdentityId(std::move(value)); return *this;}
    inline UnprocessedIdentityId& WithIdentityId(const char* value) { SetIdentityId(value); return *this;}
    ///@}

    ///@{
    /**
     * <p>The error code indicating the type of error that occurred.</p>
     */
    inline const ErrorCode& GetErrorCode() const{ return m_errorCode; }
    inline bool ErrorCodeHasBeenSet() const { return m_errorCodeHasBeenSet; }
    inline void SetErrorCode(const ErrorCode& value) { m_errorCodeHasBeenSet = true; m_errorCode = value; }
    inline void SetErrorCode(ErrorCode&& value) { m_errorCodeHasBeenSet = true; m_errorCode = std::move(value); }
    inline UnprocessedIdentityId& WithErrorCode(const ErrorCode& value) { SetErrorCode(value); return *this;}
    inline UnprocessedIdentityId& WithErrorCode(ErrorCode&& value) { SetErrorCode(std::move(value)); return *this;}
    ///@}
  private:

    Aws::String m_identityId;
    bool m_identityIdHasBeenSet = false;

    ErrorCode m_errorCode;
    bool m_errorCodeHasBeenSet = false;
  };

} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
