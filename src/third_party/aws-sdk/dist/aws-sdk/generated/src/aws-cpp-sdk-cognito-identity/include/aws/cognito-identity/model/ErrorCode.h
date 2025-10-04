/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/cognito-identity/CognitoIdentity_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace CognitoIdentity
{
namespace Model
{
  enum class ErrorCode
  {
    NOT_SET,
    AccessDenied,
    InternalServerError
  };

namespace ErrorCodeMapper
{
AWS_COGNITOIDENTITY_API ErrorCode GetErrorCodeForName(const Aws::String& name);

AWS_COGNITOIDENTITY_API Aws::String GetNameForErrorCode(ErrorCode value);
} // namespace ErrorCodeMapper
} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
