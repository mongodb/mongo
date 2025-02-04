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
  enum class IntelligentTieringAccessTier
  {
    NOT_SET,
    ARCHIVE_ACCESS,
    DEEP_ARCHIVE_ACCESS
  };

namespace IntelligentTieringAccessTierMapper
{
AWS_S3_API IntelligentTieringAccessTier GetIntelligentTieringAccessTierForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForIntelligentTieringAccessTier(IntelligentTieringAccessTier value);
} // namespace IntelligentTieringAccessTierMapper
} // namespace Model
} // namespace S3
} // namespace Aws
