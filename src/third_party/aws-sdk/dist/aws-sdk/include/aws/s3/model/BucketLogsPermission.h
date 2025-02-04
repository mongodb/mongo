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
  enum class BucketLogsPermission
  {
    NOT_SET,
    FULL_CONTROL,
    READ,
    WRITE
  };

namespace BucketLogsPermissionMapper
{
AWS_S3_API BucketLogsPermission GetBucketLogsPermissionForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForBucketLogsPermission(BucketLogsPermission value);
} // namespace BucketLogsPermissionMapper
} // namespace Model
} // namespace S3
} // namespace Aws
