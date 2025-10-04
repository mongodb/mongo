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
  enum class PolicySourceType
  {
    NOT_SET,
    user,
    group,
    role,
    aws_managed,
    user_managed,
    resource,
    none
  };

namespace PolicySourceTypeMapper
{
AWS_IAM_API PolicySourceType GetPolicySourceTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForPolicySourceType(PolicySourceType value);
} // namespace PolicySourceTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
