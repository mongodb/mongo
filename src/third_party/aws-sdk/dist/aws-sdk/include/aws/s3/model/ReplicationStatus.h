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
  enum class ReplicationStatus
  {
    NOT_SET,
    COMPLETED,
    PENDING,
    FAILED,
    REPLICA
  };

namespace ReplicationStatusMapper
{
AWS_S3_API ReplicationStatus GetReplicationStatusForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForReplicationStatus(ReplicationStatus value);
} // namespace ReplicationStatusMapper
} // namespace Model
} // namespace S3
} // namespace Aws
