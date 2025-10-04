/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/iam/IAM_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace IAM
{
namespace Model
{
  enum class PolicyScopeType
  {
    NOT_SET,
    All,
    AWS,
    Local
  };

namespace PolicyScopeTypeMapper
{
AWS_IAM_API PolicyScopeType GetPolicyScopeTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForPolicyScopeType(PolicyScopeType value);
} // namespace PolicyScopeTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
