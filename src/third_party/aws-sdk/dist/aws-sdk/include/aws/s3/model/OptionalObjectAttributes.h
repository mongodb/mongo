/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace S3
{
namespace Model
{
  enum class OptionalObjectAttributes
  {
    NOT_SET,
    RestoreStatus
  };

namespace OptionalObjectAttributesMapper
{
AWS_S3_API OptionalObjectAttributes GetOptionalObjectAttributesForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForOptionalObjectAttributes(OptionalObjectAttributes value);
} // namespace OptionalObjectAttributesMapper
} // namespace Model
} // namespace S3
} // namespace Aws
