/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace Kinesis
{
namespace Model
{
  enum class ScalingType
  {
    NOT_SET,
    UNIFORM_SCALING
  };

namespace ScalingTypeMapper
{
AWS_KINESIS_API ScalingType GetScalingTypeForName(const Aws::String& name);

AWS_KINESIS_API Aws::String GetNameForScalingType(ScalingType value);
} // namespace ScalingTypeMapper
} // namespace Model
} // namespace Kinesis
} // namespace Aws
