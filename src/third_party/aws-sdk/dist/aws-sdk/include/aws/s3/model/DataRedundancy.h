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
  enum class DataRedundancy
  {
    NOT_SET,
    SingleAvailabilityZone,
    SingleLocalZone
  };

namespace DataRedundancyMapper
{
AWS_S3_API DataRedundancy GetDataRedundancyForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForDataRedundancy(DataRedundancy value);
} // namespace DataRedundancyMapper
} // namespace Model
} // namespace S3
} // namespace Aws
