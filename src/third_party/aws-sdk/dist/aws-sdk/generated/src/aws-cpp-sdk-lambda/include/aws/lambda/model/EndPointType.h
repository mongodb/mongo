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
  enum class EndPointType
  {
    NOT_SET,
    KAFKA_BOOTSTRAP_SERVERS
  };

namespace EndPointTypeMapper
{
AWS_LAMBDA_API EndPointType GetEndPointTypeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForEndPointType(EndPointType value);
} // namespace EndPointTypeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
