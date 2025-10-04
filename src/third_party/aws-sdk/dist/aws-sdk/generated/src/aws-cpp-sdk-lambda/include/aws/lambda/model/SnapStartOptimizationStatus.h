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
  enum class SnapStartOptimizationStatus
  {
    NOT_SET,
    On,
    Off
  };

namespace SnapStartOptimizationStatusMapper
{
AWS_LAMBDA_API SnapStartOptimizationStatus GetSnapStartOptimizationStatusForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForSnapStartOptimizationStatus(SnapStartOptimizationStatus value);
} // namespace SnapStartOptimizationStatusMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
