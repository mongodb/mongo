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
  enum class LogType
  {
    NOT_SET,
    None,
    Tail
  };

namespace LogTypeMapper
{
AWS_LAMBDA_API LogType GetLogTypeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForLogType(LogType value);
} // namespace LogTypeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
