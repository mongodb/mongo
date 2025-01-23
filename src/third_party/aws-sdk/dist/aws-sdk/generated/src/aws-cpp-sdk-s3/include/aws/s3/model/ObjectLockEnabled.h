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
  enum class ObjectLockEnabled
  {
    NOT_SET,
    Enabled
  };

namespace ObjectLockEnabledMapper
{
AWS_S3_API ObjectLockEnabled GetObjectLockEnabledForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForObjectLockEnabled(ObjectLockEnabled value);
} // namespace ObjectLockEnabledMapper
} // namespace Model
} // namespace S3
} // namespace Aws
