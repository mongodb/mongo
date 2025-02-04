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
  enum class ResponseStreamingInvocationType
  {
    NOT_SET,
    RequestResponse,
    DryRun
  };

namespace ResponseStreamingInvocationTypeMapper
{
AWS_LAMBDA_API ResponseStreamingInvocationType GetResponseStreamingInvocationTypeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForResponseStreamingInvocationType(ResponseStreamingInvocationType value);
} // namespace ResponseStreamingInvocationTypeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
