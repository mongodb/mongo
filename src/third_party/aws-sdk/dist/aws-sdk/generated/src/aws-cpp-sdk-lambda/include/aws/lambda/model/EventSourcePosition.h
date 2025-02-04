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
  enum class EventSourcePosition
  {
    NOT_SET,
    TRIM_HORIZON,
    LATEST,
    AT_TIMESTAMP
  };

namespace EventSourcePositionMapper
{
AWS_LAMBDA_API EventSourcePosition GetEventSourcePositionForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForEventSourcePosition(EventSourcePosition value);
} // namespace EventSourcePositionMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
