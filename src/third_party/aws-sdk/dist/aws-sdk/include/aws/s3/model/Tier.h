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
  enum class Tier
  {
    NOT_SET,
    Standard,
    Bulk,
    Expedited
  };

namespace TierMapper
{
AWS_S3_API Tier GetTierForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForTier(Tier value);
} // namespace TierMapper
} // namespace Model
} // namespace S3
} // namespace Aws
