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
  enum class InvokeMode
  {
    NOT_SET,
    BUFFERED,
    RESPONSE_STREAM
  };

namespace InvokeModeMapper
{
AWS_LAMBDA_API InvokeMode GetInvokeModeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForInvokeMode(InvokeMode value);
} // namespace InvokeModeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
