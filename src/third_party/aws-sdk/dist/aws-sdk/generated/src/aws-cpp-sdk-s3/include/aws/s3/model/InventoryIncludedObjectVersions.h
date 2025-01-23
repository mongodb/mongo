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
  enum class InventoryIncludedObjectVersions
  {
    NOT_SET,
    All,
    Current
  };

namespace InventoryIncludedObjectVersionsMapper
{
AWS_S3_API InventoryIncludedObjectVersions GetInventoryIncludedObjectVersionsForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForInventoryIncludedObjectVersions(InventoryIncludedObjectVersions value);
} // namespace InventoryIncludedObjectVersionsMapper
} // namespace Model
} // namespace S3
} // namespace Aws
