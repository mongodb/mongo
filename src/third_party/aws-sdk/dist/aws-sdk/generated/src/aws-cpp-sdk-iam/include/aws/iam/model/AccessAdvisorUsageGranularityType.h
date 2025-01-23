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
  enum class AccessAdvisorUsageGranularityType
  {
    NOT_SET,
    SERVICE_LEVEL,
    ACTION_LEVEL
  };

namespace AccessAdvisorUsageGranularityTypeMapper
{
AWS_IAM_API AccessAdvisorUsageGranularityType GetAccessAdvisorUsageGranularityTypeForName(const Aws::String& name);

AWS_IAM_API Aws::String GetNameForAccessAdvisorUsageGranularityType(AccessAdvisorUsageGranularityType value);
} // namespace AccessAdvisorUsageGranularityTypeMapper
} // namespace Model
} // namespace IAM
} // namespace Aws
