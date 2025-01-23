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
  enum class Runtime
  {
    NOT_SET,
    nodejs,
    nodejs4_3,
    nodejs6_10,
    nodejs8_10,
    nodejs10_x,
    nodejs12_x,
    nodejs14_x,
    nodejs16_x,
    java8,
    java8_al2,
    java11,
    python2_7,
    python3_6,
    python3_7,
    python3_8,
    python3_9,
    dotnetcore1_0,
    dotnetcore2_0,
    dotnetcore2_1,
    dotnetcore3_1,
    dotnet6,
    dotnet8,
    nodejs4_3_edge,
    go1_x,
    ruby2_5,
    ruby2_7,
    provided,
    provided_al2,
    nodejs18_x,
    python3_10,
    java17,
    ruby3_2,
    ruby3_3,
    python3_11,
    nodejs20_x,
    provided_al2023,
    python3_12,
    java21,
    python3_13,
    nodejs22_x
  };

namespace RuntimeMapper
{
AWS_LAMBDA_API Runtime GetRuntimeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForRuntime(Runtime value);
} // namespace RuntimeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
