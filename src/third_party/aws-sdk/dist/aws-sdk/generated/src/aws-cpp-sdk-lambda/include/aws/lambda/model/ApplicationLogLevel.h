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
  enum class ApplicationLogLevel
  {
    NOT_SET,
    TRACE,
    DEBUG_,
    INFO,
    WARN,
    ERROR_,
    FATAL
  };

namespace ApplicationLogLevelMapper
{
AWS_LAMBDA_API ApplicationLogLevel GetApplicationLogLevelForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForApplicationLogLevel(ApplicationLogLevel value);
} // namespace ApplicationLogLevelMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
