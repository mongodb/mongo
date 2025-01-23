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
  enum class ConsumerStatus
  {
    NOT_SET,
    CREATING,
    DELETING,
    ACTIVE
  };

namespace ConsumerStatusMapper
{
AWS_KINESIS_API ConsumerStatus GetConsumerStatusForName(const Aws::String& name);

AWS_KINESIS_API Aws::String GetNameForConsumerStatus(ConsumerStatus value);
} // namespace ConsumerStatusMapper
} // namespace Model
} // namespace Kinesis
} // namespace Aws
