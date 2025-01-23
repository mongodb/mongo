/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace Lambda
{
namespace Model
{
  enum class EventSourceMappingMetric
  {
    NOT_SET,
    EventCount
  };

namespace EventSourceMappingMetricMapper
{
AWS_LAMBDA_API EventSourceMappingMetric GetEventSourceMappingMetricForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForEventSourceMappingMetric(EventSourceMappingMetric value);
} // namespace EventSourceMappingMetricMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
