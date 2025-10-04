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
  enum class PackageType
  {
    NOT_SET,
    Zip,
    Image
  };

namespace PackageTypeMapper
{
AWS_LAMBDA_API PackageType GetPackageTypeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForPackageType(PackageType value);
} // namespace PackageTypeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
