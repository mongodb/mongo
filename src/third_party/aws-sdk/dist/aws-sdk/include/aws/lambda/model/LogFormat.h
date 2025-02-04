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
  enum class LogFormat
  {
    NOT_SET,
    JSON,
    Text
  };

namespace LogFormatMapper
{
AWS_LAMBDA_API LogFormat GetLogFormatForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForLogFormat(LogFormat value);
} // namespace LogFormatMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
