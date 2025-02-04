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
  enum class State
  {
    NOT_SET,
    Pending,
    Active,
    Inactive,
    Failed
  };

namespace StateMapper
{
AWS_LAMBDA_API State GetStateForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForState(State value);
} // namespace StateMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
