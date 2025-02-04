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
  enum class AmbiguousRoleResolutionType
  {
    NOT_SET,
    AuthenticatedRole,
    Deny
  };

namespace AmbiguousRoleResolutionTypeMapper
{
AWS_COGNITOIDENTITY_API AmbiguousRoleResolutionType GetAmbiguousRoleResolutionTypeForName(const Aws::String& name);

AWS_COGNITOIDENTITY_API Aws::String GetNameForAmbiguousRoleResolutionType(AmbiguousRoleResolutionType value);
} // namespace AmbiguousRoleResolutionTypeMapper
} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
