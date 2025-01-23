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
  enum class StreamMode
  {
    NOT_SET,
    PROVISIONED,
    ON_DEMAND
  };

namespace StreamModeMapper
{
AWS_KINESIS_API StreamMode GetStreamModeForName(const Aws::String& name);

AWS_KINESIS_API Aws::String GetNameForStreamMode(StreamMode value);
} // namespace StreamModeMapper
} // namespace Model
} // namespace Kinesis
} // namespace Aws
