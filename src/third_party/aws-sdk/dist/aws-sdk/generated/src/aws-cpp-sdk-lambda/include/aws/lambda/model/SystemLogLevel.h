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
  enum class SystemLogLevel
  {
    NOT_SET,
    DEBUG_,
    INFO,
    WARN
  };

namespace SystemLogLevelMapper
{
AWS_LAMBDA_API SystemLogLevel GetSystemLogLevelForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForSystemLogLevel(SystemLogLevel value);
} // namespace SystemLogLevelMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
