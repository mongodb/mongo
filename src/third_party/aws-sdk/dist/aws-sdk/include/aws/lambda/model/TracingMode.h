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
  enum class TracingMode
  {
    NOT_SET,
    Active,
    PassThrough
  };

namespace TracingModeMapper
{
AWS_LAMBDA_API TracingMode GetTracingModeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForTracingMode(TracingMode value);
} // namespace TracingModeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
