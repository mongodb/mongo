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
  enum class InvocationType
  {
    NOT_SET,
    Event,
    RequestResponse,
    DryRun
  };

namespace InvocationTypeMapper
{
AWS_LAMBDA_API InvocationType GetInvocationTypeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForInvocationType(InvocationType value);
} // namespace InvocationTypeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
