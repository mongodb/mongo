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
  enum class FunctionVersion
  {
    NOT_SET,
    ALL
  };

namespace FunctionVersionMapper
{
AWS_LAMBDA_API FunctionVersion GetFunctionVersionForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForFunctionVersion(FunctionVersion value);
} // namespace FunctionVersionMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
