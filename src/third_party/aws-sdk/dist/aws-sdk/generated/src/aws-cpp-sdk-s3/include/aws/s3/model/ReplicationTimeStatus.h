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
  enum class ReplicationTimeStatus
  {
    NOT_SET,
    Enabled,
    Disabled
  };

namespace ReplicationTimeStatusMapper
{
AWS_S3_API ReplicationTimeStatus GetReplicationTimeStatusForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForReplicationTimeStatus(ReplicationTimeStatus value);
} // namespace ReplicationTimeStatusMapper
} // namespace Model
} // namespace S3
} // namespace Aws
