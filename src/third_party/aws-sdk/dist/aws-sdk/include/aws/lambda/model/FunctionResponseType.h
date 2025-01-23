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
  enum class FunctionResponseType
  {
    NOT_SET,
    ReportBatchItemFailures
  };

namespace FunctionResponseTypeMapper
{
AWS_LAMBDA_API FunctionResponseType GetFunctionResponseTypeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForFunctionResponseType(FunctionResponseType value);
} // namespace FunctionResponseTypeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
