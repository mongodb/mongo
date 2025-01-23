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
  enum class Architecture
  {
    NOT_SET,
    x86_64,
    arm64
  };

namespace ArchitectureMapper
{
AWS_LAMBDA_API Architecture GetArchitectureForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForArchitecture(Architecture value);
} // namespace ArchitectureMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
