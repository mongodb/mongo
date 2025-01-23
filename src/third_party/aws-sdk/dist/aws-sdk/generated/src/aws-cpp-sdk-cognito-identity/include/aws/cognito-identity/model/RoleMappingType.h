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
  enum class RoleMappingType
  {
    NOT_SET,
    Token,
    Rules
  };

namespace RoleMappingTypeMapper
{
AWS_COGNITOIDENTITY_API RoleMappingType GetRoleMappingTypeForName(const Aws::String& name);

AWS_COGNITOIDENTITY_API Aws::String GetNameForRoleMappingType(RoleMappingType value);
} // namespace RoleMappingTypeMapper
} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
