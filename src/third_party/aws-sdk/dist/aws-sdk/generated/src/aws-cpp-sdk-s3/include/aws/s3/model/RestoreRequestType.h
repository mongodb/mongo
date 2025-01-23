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
  enum class RestoreRequestType
  {
    NOT_SET,
    SELECT
  };

namespace RestoreRequestTypeMapper
{
AWS_S3_API RestoreRequestType GetRestoreRequestTypeForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForRestoreRequestType(RestoreRequestType value);
} // namespace RestoreRequestTypeMapper
} // namespace Model
} // namespace S3
} // namespace Aws
