/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace Kinesis
{
namespace Model
{
  enum class ShardFilterType
  {
    NOT_SET,
    AFTER_SHARD_ID,
    AT_TRIM_HORIZON,
    FROM_TRIM_HORIZON,
    AT_LATEST,
    AT_TIMESTAMP,
    FROM_TIMESTAMP
  };

namespace ShardFilterTypeMapper
{
AWS_KINESIS_API ShardFilterType GetShardFilterTypeForName(const Aws::String& name);

AWS_KINESIS_API Aws::String GetNameForShardFilterType(ShardFilterType value);
} // namespace ShardFilterTypeMapper
} // namespace Model
} // namespace Kinesis
} // namespace Aws
