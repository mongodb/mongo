/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace Lambda
{
namespace Model
{
  enum class ThrottleReason
  {
    NOT_SET,
    ConcurrentInvocationLimitExceeded,
    FunctionInvocationRateLimitExceeded,
    ReservedFunctionConcurrentInvocationLimitExceeded,
    ReservedFunctionInvocationRateLimitExceeded,
    CallerRateLimitExceeded,
    ConcurrentSnapshotCreateLimitExceeded
  };

namespace ThrottleReasonMapper
{
AWS_LAMBDA_API ThrottleReason GetThrottleReasonForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForThrottleReason(ThrottleReason value);
} // namespace ThrottleReasonMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
