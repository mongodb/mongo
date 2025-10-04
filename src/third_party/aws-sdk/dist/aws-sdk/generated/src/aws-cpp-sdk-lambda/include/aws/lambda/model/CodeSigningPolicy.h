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
  enum class CodeSigningPolicy
  {
    NOT_SET,
    Warn,
    Enforce
  };

namespace CodeSigningPolicyMapper
{
AWS_LAMBDA_API CodeSigningPolicy GetCodeSigningPolicyForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForCodeSigningPolicy(CodeSigningPolicy value);
} // namespace CodeSigningPolicyMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
