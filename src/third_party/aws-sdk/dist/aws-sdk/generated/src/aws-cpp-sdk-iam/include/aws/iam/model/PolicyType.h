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
  enum class PolicyType
  {
    NOT_SET,
    INLINE,
    MANAGED
  };

namespace PolicyTypeMapper
{
AWS_IAM_API PolicyType GetPolicyTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForPolicyType(PolicyType value);
} // namespace PolicyTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
