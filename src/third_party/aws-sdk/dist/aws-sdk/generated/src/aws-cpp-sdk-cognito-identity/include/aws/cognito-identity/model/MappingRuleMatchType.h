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
  enum class MappingRuleMatchType
  {
    NOT_SET,
    Equals,
    Contains,
    StartsWith,
    NotEqual
  };

namespace MappingRuleMatchTypeMapper
{
AWS_COGNITOIDENTITY_API MappingRuleMatchType GetMappingRuleMatchTypeForName(const Aws::String& name);

AWS_COGNITOIDENTITY_API Aws::String GetNameForMappingRuleMatchType(MappingRuleMatchType value);
} // namespace MappingRuleMatchTypeMapper
} // namespace Model
} // namespace CognitoIdentity
} // namespace Aws
