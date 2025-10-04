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
  enum class ShardIteratorType
  {
    NOT_SET,
    AT_SEQUENCE_NUMBER,
    AFTER_SEQUENCE_NUMBER,
    TRIM_HORIZON,
    LATEST,
    AT_TIMESTAMP
  };

namespace ShardIteratorTypeMapper
{
AWS_KINESIS_API ShardIteratorType GetShardIteratorTypeForName(const Aws::String& name);

AWS_KINESIS_API Aws::String GetNameForShardIteratorType(ShardIteratorType value);
} // namespace ShardIteratorTypeMapper
} // namespace Model
} // namespace Kinesis
} // namespace Aws
