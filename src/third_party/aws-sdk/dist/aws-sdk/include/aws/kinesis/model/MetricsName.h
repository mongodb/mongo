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
  enum class MetricsName
  {
    NOT_SET,
    IncomingBytes,
    IncomingRecords,
    OutgoingBytes,
    OutgoingRecords,
    WriteProvisionedThroughputExceeded,
    ReadProvisionedThroughputExceeded,
    IteratorAgeMilliseconds,
    ALL
  };

namespace MetricsNameMapper
{
AWS_KINESIS_API MetricsName GetMetricsNameForName(const Aws::String& name);

AWS_KINESIS_API Aws::String GetNameForMetricsName(MetricsName value);
} // namespace MetricsNameMapper
} // namespace Model
} // namespace Kinesis
} // namespace Aws
