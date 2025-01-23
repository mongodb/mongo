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
  enum class UpdateRuntimeOn
  {
    NOT_SET,
    Auto,
    Manual,
    FunctionUpdate
  };

namespace UpdateRuntimeOnMapper
{
AWS_LAMBDA_API UpdateRuntimeOn GetUpdateRuntimeOnForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForUpdateRuntimeOn(UpdateRuntimeOn value);
} // namespace UpdateRuntimeOnMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
