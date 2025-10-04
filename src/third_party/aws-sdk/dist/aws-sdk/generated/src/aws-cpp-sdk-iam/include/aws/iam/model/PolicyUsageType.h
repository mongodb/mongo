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
  enum class PolicyUsageType
  {
    NOT_SET,
    PermissionsPolicy,
    PermissionsBoundary
  };

namespace PolicyUsageTypeMapper
{
AWS_IAM_API PolicyUsageType GetPolicyUsageTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForPolicyUsageType(PolicyUsageType value);
} // namespace PolicyUsageTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
